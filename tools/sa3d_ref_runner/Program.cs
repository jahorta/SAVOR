using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using SA3D.Modeling.Animation;
using SA3D.Modeling.File;
using SA3D.Modeling.Mesh.Buffer;
using SA3D.Modeling.Mesh.Chunk;
using SA3D.Modeling.Mesh.Chunk.PolyChunks;

internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length == 0 || args[0] is "--help" or "-h")
        {
            PrintUsage();
            return 0;
        }

        var command = args[0].Trim();
        var optionValues = ParseOptions(args.Skip(1).ToArray());

        return command switch
        {
            "run-one" => RunOne(optionValues),
            "run-all" => RunAll(optionValues),
            _ => UnknownCommand(command),
        };
    }

    private static int UnknownCommand(string command)
    {
        Console.Error.WriteLine($"Unknown command: {command}");
        PrintUsage();
        return 2;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("SA3DRefRunner usage:");
        Console.WriteLine("  SA3DRefRunner run-one --input <file.mld> --out <dir> [--output-file <path>] [--manifest <path>] --block-manifest <path> [--slice <n>] [--sa3d-modeling-dll <path>]");
        Console.WriteLine("  SA3DRefRunner run-all --manifest <path> --out <dir> [--slice <n>] [--sa3d-modeling-dll <path>]");
        Console.WriteLine();
        Console.WriteLine("Defaults:");
        Console.WriteLine("  SA3D.Modeling.dll is auto-discovered next to SA3DRefRunner or under third-party/SA3D.Modeling build outputs.");
    }

    private static Dictionary<string, string> ParseOptions(string[] args)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            if (!arg.StartsWith("--", StringComparison.Ordinal))
            {
                continue;
            }

            if (i + 1 >= args.Length)
            {
                break;
            }

            values[arg] = args[i + 1];
            i++;
        }

        return values;
    }

    private static int RunOne(Dictionary<string, string> options)
    {
        if (!TryGetRequiredPath(options, "--input", out var inputPath, out var missingInputError))
        {
            Console.Error.WriteLine(missingInputError);
            return 2;
        }

        if (!File.Exists(inputPath))
        {
            Console.Error.WriteLine($"Input file does not exist: {inputPath}");
            return 1;
        }

        var outDir = options.TryGetValue("--out", out var outRaw) && !string.IsNullOrWhiteSpace(outRaw)
            ? Path.GetFullPath(outRaw)
            : Directory.GetCurrentDirectory();
        Directory.CreateDirectory(outDir);

        var outputFile = options.TryGetValue("--output-file", out var outputFileRaw) && !string.IsNullOrWhiteSpace(outputFileRaw)
            ? Path.GetFullPath(outputFileRaw)
            : Path.Combine(outDir, Path.GetFileNameWithoutExtension(inputPath) + ".sa3d.reference.json");

        var slice = options.TryGetValue("--slice", out var sliceRaw) && int.TryParse(sliceRaw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsedSlice)
            ? parsedSlice
            : 0;

        var report = BuildReportForFixture(inputPath, outputFile, outDir, options, slice);
        WriteReport(outputFile, report);
        Console.WriteLine($"Wrote reference report: {outputFile}");
        return report.Comparison.Pass ? 0 : 1;
    }

    private static int RunAll(Dictionary<string, string> options)
    {
        if (!TryGetRequiredPath(options, "--manifest", out var manifestPath, out var missingManifestError))
        {
            Console.Error.WriteLine(missingManifestError);
            return 2;
        }

        if (!TryGetRequiredPath(options, "--out", out var outDir, out var missingOutError))
        {
            Console.Error.WriteLine(missingOutError);
            return 2;
        }

        if (!File.Exists(manifestPath))
        {
            Console.Error.WriteLine($"Manifest does not exist: {manifestPath}");
            return 1;
        }

        Directory.CreateDirectory(outDir);

        var slice = options.TryGetValue("--slice", out var sliceRaw) && int.TryParse(sliceRaw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsedSlice)
            ? parsedSlice
            : 0;

        var fixtures = ResolveFixturePaths(manifestPath);
        var results = new List<BatchFixtureResult>();
        foreach (var fixture in fixtures)
        {
            var outputFile = Path.Combine(outDir, Path.GetFileNameWithoutExtension(fixture) + ".sa3d.reference.json");
            var report = BuildReportForFixture(fixture, outputFile, outDir, options, slice);
            WriteReport(outputFile, report);

            results.Add(new BatchFixtureResult
            {
                FixtureId = report.Fixture.Id,
                InputPath = fixture,
                OutputPath = outputFile,
                Pass = report.Comparison.Pass,
                ErrorCount = report.Diagnostics.Count(x => x.Severity.Equals("error", StringComparison.OrdinalIgnoreCase)),
            });

            Console.WriteLine($"[{(report.Comparison.Pass ? "PASS" : "FAIL")}] {Path.GetFileName(fixture)} => {outputFile}");
        }

        var summary = new BatchSummary
        {
            Schema = "parity_report_batch_v1",
            TotalFixtures = results.Count,
            PassedFixtures = results.Count(x => x.Pass),
            FailedFixtures = results.Count(x => !x.Pass),
            Slice = slice,
            Results = results.OrderBy(x => x.FixtureId, StringComparer.Ordinal).ToList(),
        };

        var summaryPath = Path.Combine(outDir, "summary_index.json");
        var json = JsonSerializer.Serialize(summary, JsonOptions);
        File.WriteAllText(summaryPath, json, new UTF8Encoding(false));

        Console.WriteLine($"run-all complete: {summary.PassedFixtures}/{summary.TotalFixtures} fixture(s) passed.");
        Console.WriteLine($"Wrote batch summary: {summaryPath}");
        return summary.FailedFixtures == 0 ? 0 : 1;
    }

    private static bool TryGetRequiredPath(Dictionary<string, string> options, string key, out string path, out string error)
    {
        if (!options.TryGetValue(key, out var raw) || string.IsNullOrWhiteSpace(raw))
        {
            path = string.Empty;
            error = $"Missing required option {key}";
            return false;
        }

        path = Path.GetFullPath(raw);
        error = string.Empty;
        return true;
    }

    private static ReferenceReport BuildReportForFixture(
        string inputPath,
        string outputFile,
        string outDir,
        Dictionary<string, string> options,
        int slice)
    {
        var bytes = File.ReadAllBytes(inputPath);
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var diagnostics = new List<Diagnostic>();
        var blockManifest = TryLoadBlockManifest(options, diagnostics);

        var parserOutput = InvokeSa3dModelingReference(inputPath, outDir, options, slice, blockManifest, diagnostics);

        var metrics = new Metrics
        {
            Structural = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["input_size_bytes"] = JsonSerializer.SerializeToElement(bytes.Length),
                ["reference_invoked"] = JsonSerializer.SerializeToElement(parserOutput.Invoked),
            },
            Semantic = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["input_sha256"] = JsonSerializer.SerializeToElement(hash),
                ["reference_status"] = JsonSerializer.SerializeToElement(parserOutput.Status),
            },
        };

        if (blockManifest is not null)
        {
            metrics.Structural["block_manifest_present"] = JsonSerializer.SerializeToElement(true);
            metrics.Structural["block_count"] = JsonSerializer.SerializeToElement(blockManifest.Blocks.Count);
            metrics.Structural["object_block_count"] = JsonSerializer.SerializeToElement(blockManifest.Blocks.Count(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase)));
            metrics.Structural["motion_block_count"] = JsonSerializer.SerializeToElement(blockManifest.Blocks.Count(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase)));
            metrics.Semantic["fixture_id"] = JsonSerializer.SerializeToElement(blockManifest.FixtureId ?? string.Empty);
        }
        else
        {
            metrics.Structural["block_manifest_present"] = JsonSerializer.SerializeToElement(false);
        }

        if (parserOutput.Structural is not null)
        {
            foreach (var pair in parserOutput.Structural)
            {
                metrics.Structural[pair.Key] = pair.Value;
            }
        }

        if (parserOutput.Semantic is not null)
        {
            foreach (var pair in parserOutput.Semantic)
            {
                metrics.Semantic[pair.Key] = pair.Value;
            }
        }

        var hasError = diagnostics.Any(d => d.Severity.Equals("error", StringComparison.OrdinalIgnoreCase));

        var fixtureBlobBase64 = Convert.ToBase64String(bytes);
        var slicePairs = parserOutput.CollatedSlicePairs.Count > 0
            ? parserOutput.CollatedSlicePairs
            : new List<SliceIoPair>
            {
                new()
                {
                    Slice = slice,
                    Pairs =
                    [
                        new FunctionIoPair
                        {
                            FunctionId = "sa3d.bridge.fixture_context",
                            InputFields = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                            {
                                ["fixture_blob_base64"] = JsonSerializer.SerializeToElement(fixtureBlobBase64),
                                ["fixture_blob_encoding"] = JsonSerializer.SerializeToElement("base64"),
                                ["input_size_bytes"] = JsonSerializer.SerializeToElement(bytes.Length),
                                ["input_sha256"] = JsonSerializer.SerializeToElement(hash),
                                ["block_count"] = JsonSerializer.SerializeToElement(blockManifest?.Blocks.Count ?? 0),
                            },
                            Output = JsonSerializer.SerializeToElement(parserOutput.Outputs, JsonOptions),
                        },
                    ],
                },
            };

        return new ReferenceReport
        {
            Schema = "parity_report_v1",
            Fixture = new Fixture
            {
                Id = Path.GetFileNameWithoutExtension(inputPath),
                MldPath = inputPath,
                ModelBlockOffset = parserOutput.ModelBlockOffset,
                MotionBlockOffset = parserOutput.MotionBlockOffset,
            },
            Reference = new Reference
            {
                Source = "SA3D.Modeling",
                Tag = "1.2.1",
                Commit = "13813e7",
                RunnerBranch = "DetailedIO2",
            },
            Metrics = metrics,
            Diagnostics = diagnostics.OrderBy(d => d.Code, StringComparer.Ordinal).ToList(),
            Comparison = new Comparison
            {
                Pass = !hasError,
                MismatchCount = hasError ? diagnostics.Count(x => x.Severity.Equals("error", StringComparison.OrdinalIgnoreCase)) : 0,
            },
            SliceIoPairs = slicePairs,
        };
    }

    private static BlockManifest? TryLoadBlockManifest(Dictionary<string, string> options, List<Diagnostic> diagnostics)
    {
        if (!options.TryGetValue("--block-manifest", out var blockManifestRaw) || string.IsNullOrWhiteSpace(blockManifestRaw))
        {
            return null;
        }

        var blockManifestPath = Path.GetFullPath(blockManifestRaw);
        if (!File.Exists(blockManifestPath))
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "BLOCK_MANIFEST_MISSING",
                Severity = "error",
                Stage = "block_manifest_load",
                Message = $"Block manifest does not exist: {blockManifestPath}",
            });
            return null;
        }

        try
        {
            var json = File.ReadAllText(blockManifestPath, Encoding.UTF8);
            var manifest = JsonSerializer.Deserialize<BlockManifest>(json, JsonOptions);
            if (manifest is null)
            {
                diagnostics.Add(new Diagnostic
                {
                    Code = "BLOCK_MANIFEST_INVALID",
                    Severity = "error",
                    Stage = "block_manifest_load",
                    Message = $"Block manifest JSON parsed to null: {blockManifestPath}",
                });
                return null;
            }

            var blockManifestDir = Path.GetDirectoryName(blockManifestPath) ?? Directory.GetCurrentDirectory();
            foreach (var block in manifest.Blocks)
            {
                if (string.IsNullOrWhiteSpace(block.Path))
                {
                    continue;
                }
                block.Path = Path.IsPathRooted(block.Path)
                    ? block.Path
                    : Path.GetFullPath(Path.Combine(blockManifestDir, block.Path));
            }

            var missingCount = manifest.Blocks.Count(x => string.IsNullOrWhiteSpace(x.Path) || !File.Exists(x.Path));
            if (missingCount > 0)
            {
                diagnostics.Add(new Diagnostic
                {
                    Code = "BLOCK_MANIFEST_BLOCKS_MISSING",
                    Severity = "error",
                    Stage = "block_manifest_load",
                    Message = $"Block manifest contains {missingCount} missing block file path(s).",
                });
            }
            return manifest;
        }
        catch (Exception ex)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "BLOCK_MANIFEST_PARSE_FAILED",
                Severity = "error",
                Stage = "block_manifest_load",
                Message = $"Failed to parse block manifest JSON: {ex.Message}",
            });
            return null;
        }
    }

    private static ParserInvocationResult InvokeSa3dModelingReference(
        string inputPath,
        string outDir,
        Dictionary<string, string> options,
        int slice,
        BlockManifest? blockManifest,
        List<Diagnostic> diagnostics)
    {
        if (blockManifest is null || blockManifest.Blocks.Count == 0)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "BLOCK_MANIFEST_REQUIRED",
                Severity = "error",
                Stage = "reference_invoke",
                Message = "A populated --block-manifest is required for SA3D.Modeling bridge invocation.",
            });
            return ParserInvocationResult.Failed("missing_block_manifest");
        }

        if (slice == 1)
        {
            return BuildPrimitiveProbeReferenceResult(blockManifest);
        }

        if (slice == 2)
        {
            return BuildBlockManifestReferenceResult(blockManifest);
        }

        if (slice is >= 3 and <= 9)
        {
            return BuildStagedSa3dReferenceResult(blockManifest, slice, diagnostics);
        }

        if (slice != 1)
        {
            return ParserInvocationResult.NotApplicable($"slice_{slice}_not_implemented");
        }

        if (!TryResolveSa3dModelingAssemblyPath(options, out var assemblyPath))
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "SA3D_ASSEMBLY_NOT_FOUND",
                Severity = "error",
                Stage = "reference_invoke",
                Message = "Could not locate SA3D.Modeling.dll. Provide --sa3d-modeling-dll or place it next to SA3DRefRunner.",
            });
            return ParserInvocationResult.Failed("assembly_not_found");
        }

        try
        {
            var assembly = System.Reflection.Assembly.LoadFrom(assemblyPath);
            if (TryInvokeParityReportGenerator(assembly, inputPath, slice, blockManifest, diagnostics, out var parityResult))
            {
                parityResult.Outputs["sa3d_modeling_dll"] = JsonSerializer.SerializeToElement(assemblyPath);
                return parityResult;
            }

            diagnostics.Add(new Diagnostic
            {
                Code = "SA3D_PARITY_API_NOT_FOUND",
                Severity = "warning",
                Stage = "reference_invoke",
                Message = "ParityReportGenerator API not found; falling back to ReadFromBytes reflection binding.",
            });
            return InvokeLegacyReadFromBytes(assembly, inputPath, slice, blockManifest, diagnostics, assemblyPath);
        }
        catch (Exception ex)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "SA3D_INVOKE_FAILED",
                Severity = "error",
                Stage = "reference_invoke",
                Message = $"Failed to invoke SA3D.Modeling via reflection: {ex.Message}",
            });
            return ParserInvocationResult.Failed("invoke_failed");
        }
    }

    private static ParserInvocationResult BuildPrimitiveProbeReferenceResult(BlockManifest blockManifest)
    {
        var functionPairs = new List<SliceFunctionPair>();
        var primitiveCount = 0;
        var bamsCount = 0;
        var lutCount = 0;

        foreach (var block in blockManifest.Blocks.OrderBy(x => x.Index))
        {
            if (string.IsNullOrWhiteSpace(block.Path) || !File.Exists(block.Path))
            {
                continue;
            }

            var bytes = File.ReadAllBytes(block.Path);
            foreach (var primitive in BuildPrimitiveProbePairs(block, bytes))
            {
                functionPairs.Add(new SliceFunctionPair { Slice = 1, Pair = primitive });
                primitiveCount++;
            }

            if (bytes.Length >= 2)
            {
                var bamsValue = ReadI16BigEndian(bytes, 0);
                functionPairs.Add(new SliceFunctionPair
                {
                    Slice = 1,
                    Pair = new FunctionIoPair
                    {
                        FunctionId = "Sa3Dport.Testing.Slice1TestApi.BamsCheckpoint",
                        InputFields = BuildCommonProbeInput(block, bytes, 0, "bams_to_deg", "bams16"),
                        Output = JsonSerializer.SerializeToElement(new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                        {
                            ["degrees"] = JsonSerializer.SerializeToElement(bamsValue * (360.0 / 65536.0)),
                            ["radians"] = JsonSerializer.SerializeToElement(bamsValue * (Math.PI * 2.0 / 65536.0)),
                        }, JsonOptions),
                    },
                });
                bamsCount++;
            }

            functionPairs.Add(new SliceFunctionPair
            {
                Slice = 1,
                Pair = new FunctionIoPair
                {
                    FunctionId = "Sa3Dport.Testing.Slice1TestApi.LutOp",
                    InputFields = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                    {
                        ["action"] = JsonSerializer.SerializeToElement("add"),
                        ["address"] = JsonSerializer.SerializeToElement(block.Offset),
                        ["block_index"] = JsonSerializer.SerializeToElement(block.Index),
                        ["block_kind"] = JsonSerializer.SerializeToElement(block.Kind),
                        ["category"] = JsonSerializer.SerializeToElement(block.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase) ? "motion" : "object"),
                    },
                    Output = JsonSerializer.SerializeToElement(new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                    {
                        ["memoized"] = JsonSerializer.SerializeToElement(true),
                    }, JsonOptions),
                },
            });
            lutCount++;
        }

        var structural = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("primitive_probe"),
            ["block_count"] = JsonSerializer.SerializeToElement(blockManifest.Blocks.Count),
            ["collated_slice_pairs"] = JsonSerializer.SerializeToElement(functionPairs.Count),
            ["failed_blocks"] = JsonSerializer.SerializeToElement(0),
            ["primitive_op_count"] = JsonSerializer.SerializeToElement(primitiveCount),
            ["bams_checkpoint_count"] = JsonSerializer.SerializeToElement(bamsCount),
            ["lut_op_count"] = JsonSerializer.SerializeToElement(lutCount),
            ["parity_error_blocks"] = JsonSerializer.SerializeToElement(0),
        };

        var semantic = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["capture_mode"] = JsonSerializer.SerializeToElement("primitive_probe"),
        };

        var outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("primitive_probe"),
            ["capture_mode"] = JsonSerializer.SerializeToElement("primitive_probe"),
            ["primitive_op_count"] = JsonSerializer.SerializeToElement(primitiveCount),
            ["bams_checkpoint_count"] = JsonSerializer.SerializeToElement(bamsCount),
            ["lut_op_count"] = JsonSerializer.SerializeToElement(lutCount),
            ["slice"] = JsonSerializer.SerializeToElement(1),
        };

        return new ParserInvocationResult
        {
            Invoked = true,
            Status = "ok",
            Structural = structural,
            Semantic = semantic,
            Outputs = outputs,
            ModelBlockOffset = blockManifest.Blocks
                .Where(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase))
                .Select(x => (int?)x.Offset)
                .FirstOrDefault(),
            MotionBlockOffset = blockManifest.Blocks
                .Where(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
                .Select(x => (int?)x.Offset)
                .FirstOrDefault(),
            CollatedSlicePairs = GroupFunctionPairsBySlice(functionPairs),
        };
    }

    private static IEnumerable<FunctionIoPair> BuildPrimitiveProbePairs(BlockManifestItem block, byte[] bytes)
    {
        var offsets = new SortedSet<int> { 0 };
        if (bytes.Length >= 8)
        {
            offsets.Add(4);
        }
        if (bytes.Length >= 16)
        {
            offsets.Add(8);
            offsets.Add(12);
        }
        if (bytes.Length >= 4)
        {
            offsets.Add(Math.Max(0, bytes.Length - 4));
        }

        foreach (var offset in offsets)
        {
            if (offset + 4 <= bytes.Length)
            {
                yield return BuildPrimitiveProbePair(block, bytes, offset, "u32_be", ReadU32BigEndian(bytes, offset));
                yield return BuildPrimitiveProbePair(block, bytes, offset, "u32_le", ReadU32LittleEndian(bytes, offset));
            }
            else if (offset + 2 <= bytes.Length)
            {
                yield return BuildPrimitiveProbePair(block, bytes, offset, "u16_be", ReadU16BigEndian(bytes, offset));
                yield return BuildPrimitiveProbePair(block, bytes, offset, "u16_le", ReadU16LittleEndian(bytes, offset));
            }
            else if (offset < bytes.Length)
            {
                yield return BuildPrimitiveProbePair(block, bytes, offset, "u8", bytes[offset]);
            }
        }
    }

    private static FunctionIoPair BuildPrimitiveProbePair(BlockManifestItem block, byte[] bytes, int offset, string type, object value)
    {
        return new FunctionIoPair
        {
            FunctionId = "Sa3Dport.Testing.Slice1TestApi.PrimitiveOp",
            InputFields = BuildCommonProbeInput(block, bytes, offset, "read", type),
            Output = JsonSerializer.SerializeToElement(new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["value_hint"] = JsonSerializer.SerializeToElement(value),
            }, JsonOptions),
        };
    }

    private static SortedDictionary<string, JsonElement> BuildCommonProbeInput(
        BlockManifestItem block,
        byte[] bytes,
        int localOffset,
        string operation,
        string type)
    {
        return new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["block_index"] = JsonSerializer.SerializeToElement(block.Index),
            ["block_kind"] = JsonSerializer.SerializeToElement(block.Kind),
            ["block_offset"] = JsonSerializer.SerializeToElement(block.Offset),
            ["block_size"] = JsonSerializer.SerializeToElement(block.Size),
            ["input_blob_base64"] = JsonSerializer.SerializeToElement(Convert.ToBase64String(bytes)),
            ["input_blob_encoding"] = JsonSerializer.SerializeToElement("base64"),
            ["local_offset"] = JsonSerializer.SerializeToElement(localOffset),
            ["operation"] = JsonSerializer.SerializeToElement(operation),
            ["source_offset"] = JsonSerializer.SerializeToElement(block.Offset + localOffset),
            ["type"] = JsonSerializer.SerializeToElement(type),
        };
    }

    private static ushort ReadU16BigEndian(byte[] bytes, int offset)
    {
        return (ushort)((bytes[offset] << 8) | bytes[offset + 1]);
    }

    private static ushort ReadU16LittleEndian(byte[] bytes, int offset)
    {
        return (ushort)(bytes[offset] | (bytes[offset + 1] << 8));
    }

    private static short ReadI16BigEndian(byte[] bytes, int offset)
    {
        return unchecked((short)ReadU16BigEndian(bytes, offset));
    }

    private static uint ReadU32BigEndian(byte[] bytes, int offset)
    {
        return ((uint)bytes[offset] << 24)
            | ((uint)bytes[offset + 1] << 16)
            | ((uint)bytes[offset + 2] << 8)
            | bytes[offset + 3];
    }

    private static uint ReadU32LittleEndian(byte[] bytes, int offset)
    {
        return bytes[offset]
            | ((uint)bytes[offset + 1] << 8)
            | ((uint)bytes[offset + 2] << 16)
            | ((uint)bytes[offset + 3] << 24);
    }

    private static ParserInvocationResult BuildBlockManifestReferenceResult(BlockManifest blockManifest)
    {
        var orderedBlocks = new List<SortedDictionary<string, JsonElement>>();
        var blockMapHash = FnvOffsetBasis;

        foreach (var block in blockManifest.Blocks.OrderBy(x => x.Index))
        {
            if (string.IsNullOrWhiteSpace(block.Path) || !File.Exists(block.Path))
            {
                continue;
            }

            var bytes = File.ReadAllBytes(block.Path);
            foreach (var scanned in ScanNjBlocks(bytes))
            {
                var sourceOffset = checked((uint)block.Offset + scanned.Offset);
                orderedBlocks.Add(new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                {
                    ["index"] = JsonSerializer.SerializeToElement(block.Index),
                    ["kind"] = JsonSerializer.SerializeToElement(block.Kind),
                    ["offset"] = JsonSerializer.SerializeToElement(sourceOffset),
                    ["local_offset"] = JsonSerializer.SerializeToElement(scanned.Offset),
                    ["header"] = JsonSerializer.SerializeToElement(scanned.Header),
                    ["size"] = JsonSerializer.SerializeToElement(scanned.Size),
                    ["role"] = JsonSerializer.SerializeToElement(scanned.Role),
                    ["includes_njtl_prefix"] = JsonSerializer.SerializeToElement(block.IncludesNjtlPrefix),
                });

                FnvUpdateUInt32(ref blockMapHash, (uint)block.Index);
                FnvUpdateUInt32(ref blockMapHash, sourceOffset);
                FnvUpdateUInt32(ref blockMapHash, scanned.Offset);
                FnvUpdateUInt32(ref blockMapHash, scanned.Header);
                FnvUpdateUInt32(ref blockMapHash, scanned.Size);
                FnvUpdateString(ref blockMapHash, scanned.Role);
            }
        }

        var blockArray = JsonSerializer.SerializeToElement(orderedBlocks, JsonOptions);
        var blockCount = orderedBlocks.Count;
        var objectCount = blockManifest.Blocks.Count(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase));
        var motionCount = blockManifest.Blocks.Count(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase));
        var selectionHash = ComputeJsonHash(blockArray);

        var structural = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("block_manifest"),
            ["block_count"] = JsonSerializer.SerializeToElement(blockCount),
            ["object_block_count"] = JsonSerializer.SerializeToElement(objectCount),
            ["motion_block_count"] = JsonSerializer.SerializeToElement(motionCount),
            ["failed_blocks"] = JsonSerializer.SerializeToElement(0),
            ["parity_error_blocks"] = JsonSerializer.SerializeToElement(0),
            ["collated_slice_pairs"] = JsonSerializer.SerializeToElement(1),
        };

        var semantic = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["capture_mode"] = JsonSerializer.SerializeToElement("block_manifest"),
            ["selection_hash"] = JsonSerializer.SerializeToElement(selectionHash),
            ["block_map_hash"] = JsonSerializer.SerializeToElement(ToHex64(blockMapHash)),
        };

        var outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("block_manifest"),
            ["capture_mode"] = JsonSerializer.SerializeToElement("block_manifest"),
            ["block_count"] = JsonSerializer.SerializeToElement(blockCount),
            ["object_block_count"] = JsonSerializer.SerializeToElement(objectCount),
            ["motion_block_count"] = JsonSerializer.SerializeToElement(motionCount),
            ["selection_hash"] = JsonSerializer.SerializeToElement(selectionHash),
            ["block_map_hash"] = JsonSerializer.SerializeToElement(ToHex64(blockMapHash)),
        };

        var pairInputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["blocks"] = blockArray,
            ["fixture_id"] = JsonSerializer.SerializeToElement(blockManifest.FixtureId ?? string.Empty),
            ["operation"] = JsonSerializer.SerializeToElement("nj_blocks"),
        };

        return new ParserInvocationResult
        {
            Invoked = true,
            Status = "ok",
            Structural = structural,
            Semantic = semantic,
            Outputs = outputs,
            ModelBlockOffset = blockManifest.Blocks
                .Where(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase))
                .Select(x => (int?)x.Offset)
                .FirstOrDefault(),
            MotionBlockOffset = blockManifest.Blocks
                .Where(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
                .Select(x => (int?)x.Offset)
                .FirstOrDefault(),
            CollatedSlicePairs =
            [
                new SliceIoPair
                {
                    Slice = 2,
                    Pairs =
                    [
                        new FunctionIoPair
                        {
                            FunctionId = "Sa3Dport.Testing.Slice2TestApi.NjBlocks",
                            InputFields = pairInputs,
                            Output = JsonSerializer.SerializeToElement(outputs, JsonOptions),
                        },
                    ],
                },
            ],
        };
    }

    private static ParserInvocationResult BuildStagedSa3dReferenceResult(
        BlockManifest blockManifest,
        int requestedSlice,
        List<Diagnostic> diagnostics)
    {
        var summary = BuildStagedSa3dSummary(blockManifest, diagnostics);
        var outputs = summary.ToJsonDictionary();
        outputs["parser_binding"] = JsonSerializer.SerializeToElement("sa3d_modeling_staged_summary");
        outputs["capture_mode"] = JsonSerializer.SerializeToElement($"slice_{requestedSlice}_summary");

        var structural = new Dictionary<string, JsonElement>(summary.ToJsonDictionary(), StringComparer.Ordinal);
        structural["parser_binding"] = JsonSerializer.SerializeToElement("sa3d_modeling_staged_summary");
        structural["collated_slice_pairs"] = JsonSerializer.SerializeToElement(1);

        var semantic = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["capture_mode"] = JsonSerializer.SerializeToElement($"slice_{requestedSlice}_summary"),
            ["slice3_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice3StructuralHash),
            ["slice4_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice4StructuralHash),
            ["slice5_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice5StructuralHash),
            ["slice6_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice6StructuralHash),
            ["slice7_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice7StructuralHash),
            ["slice8_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice8StructuralHash),
            ["slice9_structural_hash"] = JsonSerializer.SerializeToElement(summary.Slice9StructuralHash),
        };

        return new ParserInvocationResult
        {
            Invoked = true,
            Status = summary.DiagnosticCount == 0 ? "ok" : "summary_with_diagnostics",
            Structural = structural,
            Semantic = semantic,
            Outputs = outputs,
            ModelBlockOffset = blockManifest.Blocks
                .Where(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase))
                .Select(x => (int?)x.Offset)
                .FirstOrDefault(),
            MotionBlockOffset = blockManifest.Blocks
                .Where(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
                .Select(x => (int?)x.Offset)
                .FirstOrDefault(),
            CollatedSlicePairs =
            [
                new SliceIoPair
                {
                    Slice = requestedSlice,
                    Pairs =
                    [
                        new FunctionIoPair
                        {
                            FunctionId = requestedSlice switch
                            {
                                3 => "Sa3Dport.Testing.Slice3TestApi.ReadNode",
                                4 => "Sa3Dport.Testing.Slice4TestApi.ReadChunkAttach",
                                5 => "Sa3Dport.Testing.Slice5TestApi.ReadPolyChunks",
                                6 => "Sa3Dport.Testing.Slice6TestApi.ReadModelFile",
                                7 => "Sa3Dport.Testing.Slice7TestApi.ReadMotionBlock",
                                8 => "Sa3Dport.Testing.Slice8TestApi.ReadAnimationFile",
                                9 => "Sa3Dport.Testing.Slice9TestApi.NormalizeMeshData",
                                _ => "Sa3Dport.Testing.StagedSummary",
                            },
                            InputFields = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                            {
                                ["fixture_id"] = JsonSerializer.SerializeToElement(blockManifest.FixtureId ?? string.Empty),
                                ["operation"] = JsonSerializer.SerializeToElement($"slice_{requestedSlice}_summary"),
                            },
                            Output = JsonSerializer.SerializeToElement(outputs, JsonOptions),
                        },
                    ],
                },
            ],
        };
    }

    private static StagedSa3dSummary BuildStagedSa3dSummary(BlockManifest blockManifest, List<Diagnostic> diagnostics)
    {
        var summary = new StagedSa3dSummary();
        var slice3Hash = FnvOffsetBasis;
        var slice4Hash = FnvOffsetBasis;
        var slice5Hash = FnvOffsetBasis;
        var slice5TypeHash = FnvOffsetBasis;
        var slice5AttributeHash = FnvOffsetBasis;
        var slice5ByteSizeHash = FnvOffsetBasis;
        var slice5StripMetaHash = FnvOffsetBasis;
        var slice6Hash = FnvOffsetBasis;
        var slice7Hash = FnvOffsetBasis;
        var slice8Hash = FnvOffsetBasis;
        var slice9Hash = FnvOffsetBasis;
        uint? lastModelNodeCount = null;

        foreach (var block in blockManifest.Blocks.OrderBy(x => x.Index))
        {
            if (string.IsNullOrWhiteSpace(block.Path) || !File.Exists(block.Path))
            {
                continue;
            }

            var bytes = File.ReadAllBytes(block.Path);
            foreach (var scanned in ScanNjBlocks(bytes))
            {
                if (scanned.Role.Equals("animation", StringComparison.OrdinalIgnoreCase))
                {
                    summary.Slice7MotionBlockCount++;
                    summary.Slice8AnimationFileCheckCount++;
                    if (lastModelNodeCount is null)
                    {
                        summary.DiagnosticCount++;
                        diagnostics.Add(new Diagnostic
                        {
                            Code = "SA3D_SLICE7_NODE_COUNT_MISSING",
                            Severity = "warning",
                            Stage = "reference_summary",
                            Message = $"block index={block.Index} kind={block.Kind}: no preceding model node count",
                        });
                        continue;
                    }

                    try
                    {
                        var bigEndian = CheckBigEndian32(bytes, checked((int)scanned.Offset + 4));
                        var animation = ReadSafeMotionSummary(bytes, scanned.Offset, lastModelNodeCount.Value, false, bigEndian);
                        FnvUpdateUInt32(ref slice7Hash, (uint)block.Index);
                        FnvUpdateUInt32(ref slice7Hash, scanned.Offset);
                        UpdateSlice7SummaryWithMotionSummary(summary, ref slice7Hash, animation);

                        FnvUpdateUInt32(ref slice8Hash, (uint)block.Index);
                        FnvUpdateUInt32(ref slice8Hash, scanned.Offset);
                        FnvUpdateUInt32(ref slice8Hash, scanned.Offset);
                        UpdateSlice8SummaryWithMotionSummary(summary, ref slice8Hash, animation);
                    }
                    catch (Exception ex)
                    {
                        summary.DiagnosticCount++;
                        diagnostics.Add(new Diagnostic
                        {
                            Code = "SA3D_SLICE7_SUMMARY_FAILED",
                            Severity = "warning",
                            Stage = "reference_summary",
                            Message = $"block index={block.Index} kind={block.Kind}: {ex.Message}",
                        });
                    }
                    continue;
                }

                if (!scanned.Role.Equals("model", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                summary.Slice3ModelBlockCount++;
                try
                {
                    if (ModelFile.CheckIsModelFile(bytes))
                    {
                        summary.Slice6ModelFileCheckCount++;
                    }
                    var model = ModelFile.ReadFromBytes(bytes);
                    summary.Slice3ParsedModelCount++;
                    summary.Slice6ParsedModelFileCount++;
                    lastModelNodeCount = (uint)model.Model.GetTreeNodeEnumerable().Count();
                    FnvUpdateUInt32(ref slice3Hash, (uint)block.Index);
                    FnvUpdateUInt32(ref slice3Hash, scanned.Offset);
                    FnvUpdateUInt32(ref slice4Hash, (uint)block.Index);
                    FnvUpdateUInt32(ref slice4Hash, scanned.Offset);
                    FnvUpdateUInt32(ref slice5Hash, (uint)block.Index);
                    FnvUpdateUInt32(ref slice5Hash, scanned.Offset);
                    FnvUpdateUInt32(ref slice6Hash, (uint)block.Index);
                    FnvUpdateUInt32(ref slice6Hash, scanned.Offset);
                    FnvUpdateUInt32(ref slice6Hash, (uint)model.Format);
                    FnvUpdateUInt32(ref slice9Hash, (uint)block.Index);
                    FnvUpdateUInt32(ref slice9Hash, scanned.Offset);

                    foreach (var node in model.Model.GetTreeNodeEnumerable())
                    {
                        UpdateSummaryWithNode(
                            summary,
                            ref slice3Hash,
                            ref slice4Hash,
                            ref slice5Hash,
                            ref slice5TypeHash,
                            ref slice5AttributeHash,
                            ref slice5ByteSizeHash,
                            ref slice5StripMetaHash,
                            node);
                        UpdateSlice6SummaryWithNode(summary, ref slice6Hash, node);
                    }

                    try
                    {
                        model.Model.BufferMeshData(false);
                        var slice9Summary = BuildSlice9NormalizationSummary(model.Model);
                        UpdateSlice9Summary(summary, ref slice9Hash, slice9Summary);
                    }
                    catch (Exception ex)
                    {
                        summary.DiagnosticCount++;
                        diagnostics.Add(new Diagnostic
                        {
                            Code = "SA3D_SLICE9_SUMMARY_FAILED",
                            Severity = "error",
                            Stage = "reference_summary",
                            Message = $"block index={block.Index} kind={block.Kind}: {ex.Message}",
                        });
                    }
                }
                catch (Exception ex)
                {
                    summary.DiagnosticCount++;
                    diagnostics.Add(new Diagnostic
                    {
                        Code = "SA3D_STAGED_SUMMARY_FAILED",
                        Severity = "error",
                        Stage = "reference_summary",
                        Message = $"block index={block.Index} kind={block.Kind}: {ex.Message}",
                    });
                }
            }
        }

        summary.Slice3StructuralHash = ToHex64(slice3Hash);
        summary.Slice4StructuralHash = ToHex64(slice4Hash);
        summary.Slice5StructuralHash = ToHex64(slice5Hash);
        summary.Slice5TypeHash = ToHex64(slice5TypeHash);
        summary.Slice5AttributeHash = ToHex64(slice5AttributeHash);
        summary.Slice5ByteSizeHash = ToHex64(slice5ByteSizeHash);
        summary.Slice5StripMetaHash = ToHex64(slice5StripMetaHash);
        summary.Slice6StructuralHash = ToHex64(slice6Hash);
        summary.Slice7StructuralHash = ToHex64(slice7Hash);
        summary.Slice8StructuralHash = ToHex64(slice8Hash);
        summary.Slice9StructuralHash = ToHex64(slice9Hash);
        return summary;
    }

    private static void UpdateSummaryWithNode(
        StagedSa3dSummary summary,
        ref ulong slice3Hash,
        ref ulong slice4Hash,
        ref ulong slice5Hash,
        ref ulong slice5TypeHash,
        ref ulong slice5AttributeHash,
        ref ulong slice5ByteSizeHash,
        ref ulong slice5StripMetaHash,
        SA3D.Modeling.ObjectData.Node node)
    {
        summary.Slice3NodeCount++;
        FnvUpdateUInt32(ref slice3Hash, (uint)node.Attributes);
        FnvUpdateUInt32(ref slice3Hash, node.Attach is null ? 0u : 1u);
        FnvUpdateUInt32(ref slice3Hash, node.Child is null ? 0u : 1u);
        FnvUpdateUInt32(ref slice3Hash, node.Next is null ? 0u : 1u);

        if (node.Attach is not null)
        {
            summary.Slice3AttachRefCount++;
        }

        if (node.Attach is not ChunkAttach attach)
        {
            return;
        }

        summary.Slice4ChunkAttachCount++;
        FnvUpdateUInt32(ref slice4Hash, (uint)(attach.VertexChunks?.Length ?? 0));
        FnvUpdateUInt32(ref slice4Hash, (uint)(attach.PolyChunks?.Length ?? 0));

        if (attach.VertexChunks is not null)
        {
            foreach (var vertexChunk in attach.VertexChunks)
            {
                if (vertexChunk is null)
                {
                    FnvUpdateUInt32(ref slice4Hash, 0);
                    continue;
                }

                summary.Slice4VertexChunkCount++;
                summary.Slice4VertexCount += vertexChunk.Vertices.Length;
                if (vertexChunk.HasWeight)
                {
                    summary.Slice4WeightedVertexChunkCount++;
                }
                FnvUpdateUInt32(ref slice4Hash, (uint)vertexChunk.Type);
                FnvUpdateUInt32(ref slice4Hash, vertexChunk.Attributes);
                FnvUpdateUInt32(ref slice4Hash, vertexChunk.IndexOffset);
                FnvUpdateUInt32(ref slice4Hash, (uint)vertexChunk.Vertices.Length);
            }
        }

        if (attach.PolyChunks is null)
        {
            return;
        }

        foreach (var polyChunk in attach.PolyChunks)
        {
            if (polyChunk is null)
            {
                summary.Slice5NullPolyChunkCount++;
                FnvUpdateUInt32(ref slice5Hash, 0);
                continue;
            }

            summary.Slice5PolyChunkCount++;
            FnvUpdateUInt32(ref slice5Hash, (uint)polyChunk.Type);
            FnvUpdateUInt32(ref slice5Hash, polyChunk.Attributes);
            FnvUpdateUInt32(ref slice5Hash, polyChunk.ByteSize);
            FnvUpdateUInt32(ref slice5TypeHash, (uint)polyChunk.Type);
            FnvUpdateUInt32(ref slice5AttributeHash, polyChunk.Attributes);
            FnvUpdateUInt32(ref slice5ByteSizeHash, polyChunk.ByteSize);

            switch (polyChunk)
            {
                case BitsChunk:
                    summary.Slice5BitsChunkCount++;
                    break;
                case TextureChunk:
                    summary.Slice5TextureChunkCount++;
                    break;
                case MaterialBumpChunk:
                    summary.Slice5MaterialBumpChunkCount++;
                    break;
                case MaterialChunk:
                    summary.Slice5MaterialChunkCount++;
                    break;
                case StripChunk strip:
                    summary.Slice5StripChunkCount++;
                    FnvUpdateUInt32(ref slice5Hash, (uint)strip.Strips.Length);
                    FnvUpdateUInt32(ref slice5Hash, (uint)strip.TriangleAttributeCount);
                    FnvUpdateUInt32(ref slice5StripMetaHash, (uint)strip.Strips.Length);
                    FnvUpdateUInt32(ref slice5StripMetaHash, (uint)strip.TriangleAttributeCount);
                    foreach (var stripData in strip.Strips)
                    {
                        summary.Slice5PolyCornerCount += stripData.Corners.Length;
                        FnvUpdateUInt32(ref slice5Hash, (uint)stripData.Corners.Length);
                        FnvUpdateUInt32(ref slice5StripMetaHash, (uint)stripData.Corners.Length);
                    }
                    break;
            }
        }
    }

    private static void UpdateSlice6SummaryWithNode(
        StagedSa3dSummary summary,
        ref ulong slice6Hash,
        SA3D.Modeling.ObjectData.Node node)
    {
        summary.Slice6NodeCount++;
        FnvUpdateUInt32(ref slice6Hash, (uint)node.Attributes);
        FnvUpdateUInt32(ref slice6Hash, node.Attach is null ? 0u : 1u);
        FnvUpdateUInt32(ref slice6Hash, node.Child is null ? 0u : 1u);
        FnvUpdateUInt32(ref slice6Hash, node.Next is null ? 0u : 1u);

        if (node.Attach is not null)
        {
            summary.Slice6AttachRefCount++;
        }

        if (node.Attach is not ChunkAttach attach)
        {
            return;
        }

        summary.Slice6ChunkAttachCount++;
        FnvUpdateUInt32(ref slice6Hash, (uint)(attach.VertexChunks?.Length ?? 0));
        FnvUpdateUInt32(ref slice6Hash, (uint)(attach.PolyChunks?.Length ?? 0));

        if (attach.PolyChunks is null)
        {
            return;
        }

        foreach (var polyChunk in attach.PolyChunks)
        {
            if (polyChunk is null)
            {
                FnvUpdateUInt32(ref slice6Hash, 0);
                continue;
            }

            summary.Slice6PolyChunkCount++;
            FnvUpdateUInt32(ref slice6Hash, (uint)polyChunk.Type);
            FnvUpdateUInt32(ref slice6Hash, polyChunk.Attributes);
            FnvUpdateUInt32(ref slice6Hash, polyChunk.ByteSize);
        }
    }

    private static void UpdateSlice7SummaryWithMotion(
        StagedSa3dSummary summary,
        ref ulong slice7Hash,
        Motion motion)
    {
        summary.Slice7ParsedMotionCount++;
        summary.Slice7NodeCount += (int)motion.NodeCount;
        FnvUpdateUInt32(ref slice7Hash, motion.NodeCount);
        FnvUpdateUInt32(ref slice7Hash, (uint)motion.InterpolationMode);
        FnvUpdateUInt32(ref slice7Hash, motion.ShortRot ? 1u : 0u);
        FnvUpdateUInt32(ref slice7Hash, (uint)motion.ManualKeyframeTypes);
        FnvUpdateUInt32(ref slice7Hash, (uint)motion.KeyframeTypes);
        FnvUpdateUInt32(ref slice7Hash, motion.GetFrameCount());

        foreach (var pair in motion.Keyframes.OrderBy(x => x.Key))
        {
            summary.Slice7KeyframeSetCount++;
            FnvUpdateUInt32(ref slice7Hash, (uint)pair.Key);
            FnvUpdateUInt32(ref slice7Hash, (uint)pair.Value.Type);
            FnvUpdateUInt32(ref slice7Hash, pair.Value.KeyframeCount);
            var channels = GetKeyframeChannels(pair.Value).ToList();
            FnvUpdateUInt32(ref slice7Hash, (uint)channels.Count);
            foreach (var channel in channels)
            {
                summary.Slice7ChannelCount++;
                summary.Slice7KeyframeCount += channel.Count;
                FnvUpdateUInt32(ref slice7Hash, (uint)channel.Type);
                FnvUpdateUInt32(ref slice7Hash, (uint)channel.Count);
                FnvUpdateUInt32(ref slice7Hash, channel.FirstFrame);
                FnvUpdateUInt32(ref slice7Hash, channel.LastFrame);
            }
        }
    }

    private static void UpdateSlice8SummaryWithMotion(
        StagedSa3dSummary summary,
        ref ulong slice8Hash,
        Motion motion)
    {
        summary.Slice8ParsedAnimationFileCount++;
        summary.Slice8NodeCount += (int)motion.NodeCount;
        FnvUpdateUInt32(ref slice8Hash, motion.NodeCount);
        FnvUpdateUInt32(ref slice8Hash, (uint)motion.InterpolationMode);
        FnvUpdateUInt32(ref slice8Hash, motion.ShortRot ? 1u : 0u);
        FnvUpdateUInt32(ref slice8Hash, (uint)motion.ManualKeyframeTypes);
        FnvUpdateUInt32(ref slice8Hash, (uint)motion.KeyframeTypes);
        FnvUpdateUInt32(ref slice8Hash, motion.GetFrameCount());

        foreach (var pair in motion.Keyframes.OrderBy(x => x.Key))
        {
            summary.Slice8KeyframeSetCount++;
            FnvUpdateUInt32(ref slice8Hash, (uint)pair.Key);
            FnvUpdateUInt32(ref slice8Hash, (uint)pair.Value.Type);
            FnvUpdateUInt32(ref slice8Hash, pair.Value.KeyframeCount);
            var channels = GetKeyframeChannels(pair.Value).ToList();
            FnvUpdateUInt32(ref slice8Hash, (uint)channels.Count);
            foreach (var channel in channels)
            {
                summary.Slice8ChannelCount++;
                summary.Slice8KeyframeCount += channel.Count;
                FnvUpdateUInt32(ref slice8Hash, (uint)channel.Type);
                FnvUpdateUInt32(ref slice8Hash, (uint)channel.Count);
                FnvUpdateUInt32(ref slice8Hash, channel.FirstFrame);
                FnvUpdateUInt32(ref slice8Hash, channel.LastFrame);
            }
        }
    }

    private static SafeMotionSummary ReadSafeMotionSummary(byte[] bytes, uint blockAddress, uint nodeCount, bool shortRot, bool bigEndian)
    {
        var dataAddress = checked(blockAddress + 8);
        var imageBase = unchecked(0u - dataAddress);
        var keyframeAddress = ReadMotionPointer(bytes, dataAddress, imageBase, bigEndian) ?? 0;
        var keyframeType = (ushort)ReadU16Checked(bytes, checked((int)dataAddress + 8), bigEndian);
        var tmp = ReadU16Checked(bytes, checked((int)dataAddress + 10), bigEndian);

        var result = new SafeMotionSummary
        {
            NodeCount = nodeCount,
            InterpolationMode = (uint)((tmp >> 6) & 0x3),
            ShortRot = shortRot,
            ManualKeyframeTypes = keyframeType,
        };

        var tableAddress = keyframeAddress;
        for (uint i = 0; i < nodeCount; i++)
        {
            var keyframes = ReadSafeKeyframes(bytes, ref tableAddress, keyframeType, imageBase, shortRot, bigEndian);
            result.Keyframes.Add(new SafeKeyframesSummary((int)i, keyframes.Type, keyframes.KeyframeCount, keyframes.Channels));
            result.KeyframeTypes |= keyframes.Type;
            result.FrameCount = Math.Max(result.FrameCount, keyframes.KeyframeCount);
        }

        result.KeyframeTypes |= keyframeType;
        return result;
    }

    private static SafeKeyframesSummary ReadSafeKeyframes(
        byte[] bytes,
        ref uint address,
        ushort type,
        uint imageBase,
        bool shortRot,
        bool bigEndian)
    {
        var channels = ChannelCount(type);
        var keyframePointerArray = address;
        var keyframeCountArray = checked(address + (uint)(4 * channels));
        var resultType = (ushort)0;
        var keyframeCount = 0u;
        var resultChannels = new List<SafeKeyframeChannelSummary>();

        foreach (var flag in KeyframeAttributeOrder)
        {
            if ((type & flag) == 0)
            {
                continue;
            }

            var setAddress = ReadMotionPointer(bytes, keyframePointerArray, imageBase, bigEndian);
            if (setAddress is not null)
            {
                var frameCount = ReadU32Checked(bytes, checked((int)keyframeCountArray), bigEndian);
                var firstFrame = 0u;
                var lastFrame = 0u;
                if (frameCount > 0)
                {
                    firstFrame = ReadFrameAt(bytes, setAddress.Value, flag, shortRot, bigEndian);
                    var lastAddress = setAddress.Value;
                    for (uint i = 0; i < frameCount; i++)
                    {
                        lastFrame = ReadFrameAt(bytes, lastAddress, flag, shortRot, bigEndian);
                        lastAddress += flag == KeyframeEulerRotation && shortRot
                            ? 8u
                            : KeyframeEntrySize(flag);
                    }

                    resultType |= flag;
                    keyframeCount = Math.Max(keyframeCount, lastFrame + 1);
                    resultChannels.Add(new SafeKeyframeChannelSummary(flag, frameCount, firstFrame, lastFrame));
                }
            }

            keyframePointerArray += 4;
            keyframeCountArray += 4;
        }

        address = keyframeCountArray;
        return new SafeKeyframesSummary(-1, resultType, keyframeCount, resultChannels);
    }

    private static void UpdateSlice7SummaryWithMotionSummary(
        StagedSa3dSummary summary,
        ref ulong slice7Hash,
        SafeMotionSummary motion)
    {
        summary.Slice7ParsedMotionCount++;
        summary.Slice7NodeCount += (int)motion.NodeCount;
        FnvUpdateUInt32(ref slice7Hash, motion.NodeCount);
        FnvUpdateUInt32(ref slice7Hash, motion.InterpolationMode);
        FnvUpdateUInt32(ref slice7Hash, motion.ShortRot ? 1u : 0u);
        FnvUpdateUInt32(ref slice7Hash, motion.ManualKeyframeTypes);
        FnvUpdateUInt32(ref slice7Hash, motion.KeyframeTypes);
        FnvUpdateUInt32(ref slice7Hash, motion.FrameCount);

        foreach (var keyframes in motion.Keyframes.OrderBy(x => x.NodeIndex))
        {
            summary.Slice7KeyframeSetCount++;
            FnvUpdateUInt32(ref slice7Hash, (uint)keyframes.NodeIndex);
            FnvUpdateUInt32(ref slice7Hash, keyframes.Type);
            FnvUpdateUInt32(ref slice7Hash, keyframes.KeyframeCount);
            FnvUpdateUInt32(ref slice7Hash, (uint)keyframes.Channels.Count);
            foreach (var channel in keyframes.Channels)
            {
                summary.Slice7ChannelCount++;
                summary.Slice7KeyframeCount += (int)channel.Count;
                FnvUpdateUInt32(ref slice7Hash, channel.Type);
                FnvUpdateUInt32(ref slice7Hash, channel.Count);
                FnvUpdateUInt32(ref slice7Hash, channel.FirstFrame);
                FnvUpdateUInt32(ref slice7Hash, channel.LastFrame);
            }
        }
    }

    private static void UpdateSlice8SummaryWithMotionSummary(
        StagedSa3dSummary summary,
        ref ulong slice8Hash,
        SafeMotionSummary motion)
    {
        summary.Slice8ParsedAnimationFileCount++;
        summary.Slice8NodeCount += (int)motion.NodeCount;
        FnvUpdateUInt32(ref slice8Hash, motion.NodeCount);
        FnvUpdateUInt32(ref slice8Hash, motion.InterpolationMode);
        FnvUpdateUInt32(ref slice8Hash, motion.ShortRot ? 1u : 0u);
        FnvUpdateUInt32(ref slice8Hash, motion.ManualKeyframeTypes);
        FnvUpdateUInt32(ref slice8Hash, motion.KeyframeTypes);
        FnvUpdateUInt32(ref slice8Hash, motion.FrameCount);

        foreach (var keyframes in motion.Keyframes.OrderBy(x => x.NodeIndex))
        {
            summary.Slice8KeyframeSetCount++;
            FnvUpdateUInt32(ref slice8Hash, (uint)keyframes.NodeIndex);
            FnvUpdateUInt32(ref slice8Hash, keyframes.Type);
            FnvUpdateUInt32(ref slice8Hash, keyframes.KeyframeCount);
            FnvUpdateUInt32(ref slice8Hash, (uint)keyframes.Channels.Count);
            foreach (var channel in keyframes.Channels)
            {
                summary.Slice8ChannelCount++;
                summary.Slice8KeyframeCount += (int)channel.Count;
                FnvUpdateUInt32(ref slice8Hash, channel.Type);
                FnvUpdateUInt32(ref slice8Hash, channel.Count);
                FnvUpdateUInt32(ref slice8Hash, channel.FirstFrame);
                FnvUpdateUInt32(ref slice8Hash, channel.LastFrame);
            }
        }
    }

    private static uint? ReadMotionPointer(byte[] bytes, uint address, uint imageBase, bool bigEndian)
    {
        var raw = ReadU32Checked(bytes, checked((int)address), bigEndian);
        return raw == 0 ? null : unchecked(raw - imageBase);
    }

    private static uint ReadFrameAt(byte[] bytes, uint address, ushort type, bool shortRot, bool bigEndian)
    {
        return type == KeyframeEulerRotation && shortRot
            ? ReadU16Checked(bytes, checked((int)address), bigEndian)
            : ReadU32Checked(bytes, checked((int)address), bigEndian);
    }

    private static uint KeyframeEntrySize(ushort type)
    {
        return type switch
        {
            KeyframePosition or KeyframeScale or KeyframeVector or KeyframeTarget => 16,
            KeyframeEulerRotation => 16,
            KeyframeVertex or KeyframeNormal or KeyframeRoll or KeyframeAngle or KeyframeLightColor or KeyframeIntensity => 8,
            KeyframeSpot => 24,
            KeyframePoint => 12,
            KeyframeQuaternionRotation => 20,
            _ => 0,
        };
    }

    private static int ChannelCount(ushort attributes)
    {
        var value = attributes & 0x3FFF;
        var count = 0;
        while (value != 0)
        {
            count += value & 1;
            value >>= 1;
        }
        return count;
    }

    private static ushort ReadU16Checked(byte[] bytes, int offset, bool bigEndian)
    {
        if (offset < 0 || offset + 2 > bytes.Length)
        {
            throw new InvalidOperationException("read beyond end of buffer");
        }
        return bigEndian ? ReadU16BigEndian(bytes, offset) : ReadU16LittleEndian(bytes, offset);
    }

    private static uint ReadU32Checked(byte[] bytes, int offset, bool bigEndian)
    {
        if (offset < 0 || offset + 4 > bytes.Length)
        {
            throw new InvalidOperationException("read beyond end of buffer");
        }
        return bigEndian ? ReadU32BigEndian(bytes, offset) : ReadU32LittleEndian(bytes, offset);
    }

    private static Slice9NormalizationSummary BuildSlice9NormalizationSummary(SA3D.Modeling.ObjectData.Node root)
    {
        var result = new Slice9NormalizationSummary();

        foreach (var node in root.GetTreeNodeEnumerable())
        {
            if (node.Attach is null || node.Attach.MeshData.Length == 0)
            {
                continue;
            }

            result.AttachCount++;
            var meshes = node.Attach.MeshData;
            result.BufferMeshCount += meshes.Length;

            foreach (var mesh in meshes)
            {
                result.BufferVertexCount += mesh.Vertices?.Length ?? 0;
                result.BufferCornerCount += mesh.Corners?.Length ?? 0;
                if (mesh.Corners is not null)
                {
                    result.BufferTriangleCornerCount += mesh.GetCornerTriangleList().Length;
                }
            }

            var weighted = SummarizeBufferMeshesForSlice9(meshes);
            if (weighted.VertexCount > 0 || weighted.TriangleCornerCount > 0)
            {
                result.WeightedMeshCount++;
                result.WeightedVertexCount += weighted.VertexCount;
                result.WeightedTriangleSetCount += weighted.TriangleSetCount;
                result.WeightedTriangleCornerCount += weighted.TriangleCornerCount;
            }
        }

        return result;
    }

    private static Slice9WeightedSummary SummarizeBufferMeshesForSlice9(IEnumerable<BufferMesh> meshes)
    {
        var result = new Slice9WeightedSummary();
        var usedVertices = new SortedSet<ushort>();

        foreach (var mesh in meshes)
        {
            if (mesh.Corners is null)
            {
                continue;
            }

            result.TriangleSetCount++;
            result.TriangleCornerCount += mesh.GetCornerTriangleList().Length;
            foreach (var corner in mesh.Corners)
            {
                usedVertices.Add((ushort)(corner.VertexIndex + mesh.VertexReadOffset));
            }
        }

        result.VertexCount = usedVertices.Count;
        return result;
    }

    private static void UpdateSlice9Summary(
        StagedSa3dSummary summary,
        ref ulong slice9Hash,
        Slice9NormalizationSummary normalization)
    {
        summary.Slice9AttachCount += normalization.AttachCount;
        summary.Slice9BufferMeshCount += normalization.BufferMeshCount;
        summary.Slice9BufferVertexCount += normalization.BufferVertexCount;
        summary.Slice9BufferCornerCount += normalization.BufferCornerCount;
        summary.Slice9BufferTriangleCornerCount += normalization.BufferTriangleCornerCount;
        summary.Slice9WeightedMeshCount += normalization.WeightedMeshCount;
        summary.Slice9WeightedVertexCount += normalization.WeightedVertexCount;
        summary.Slice9WeightedTriangleSetCount += normalization.WeightedTriangleSetCount;
        summary.Slice9WeightedTriangleCornerCount += normalization.WeightedTriangleCornerCount;

        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.AttachCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.BufferMeshCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.BufferVertexCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.BufferCornerCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.BufferTriangleCornerCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.WeightedMeshCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.WeightedVertexCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.WeightedTriangleSetCount);
        FnvUpdateUInt32(ref slice9Hash, (uint)normalization.WeightedTriangleCornerCount);
    }

    private static IEnumerable<KeyframeChannelSummary> GetKeyframeChannels(Keyframes keyframes)
    {
        if (keyframes.Position.Count > 0) yield return Channel(KeyframeAttributes.Position, keyframes.Position.Keys);
        if (keyframes.EulerRotation.Count > 0) yield return Channel(KeyframeAttributes.EulerRotation, keyframes.EulerRotation.Keys);
        if (keyframes.Scale.Count > 0) yield return Channel(KeyframeAttributes.Scale, keyframes.Scale.Keys);
        if (keyframes.Vector.Count > 0) yield return Channel(KeyframeAttributes.Vector, keyframes.Vector.Keys);
        if (keyframes.Vertex.Count > 0) yield return Channel(KeyframeAttributes.Vertex, keyframes.Vertex.Keys);
        if (keyframes.Normal.Count > 0) yield return Channel(KeyframeAttributes.Normal, keyframes.Normal.Keys);
        if (keyframes.Target.Count > 0) yield return Channel(KeyframeAttributes.Target, keyframes.Target.Keys);
        if (keyframes.Roll.Count > 0) yield return Channel(KeyframeAttributes.Roll, keyframes.Roll.Keys);
        if (keyframes.Angle.Count > 0) yield return Channel(KeyframeAttributes.Angle, keyframes.Angle.Keys);
        if (keyframes.LightColor.Count > 0) yield return Channel(KeyframeAttributes.LightColor, keyframes.LightColor.Keys);
        if (keyframes.Intensity.Count > 0) yield return Channel(KeyframeAttributes.Intensity, keyframes.Intensity.Keys);
        if (keyframes.Spot.Count > 0) yield return Channel(KeyframeAttributes.Spot, keyframes.Spot.Keys);
        if (keyframes.Point.Count > 0) yield return Channel(KeyframeAttributes.Point, keyframes.Point.Keys);
        if (keyframes.QuaternionRotation.Count > 0) yield return Channel(KeyframeAttributes.QuaternionRotation, keyframes.QuaternionRotation.Keys);
    }

    private static KeyframeChannelSummary Channel(KeyframeAttributes type, IEnumerable<uint> frames)
    {
        var ordered = frames.Order().ToArray();
        return new KeyframeChannelSummary(type, ordered.Length, ordered.FirstOrDefault(), ordered.LastOrDefault());
    }

    private readonly record struct KeyframeChannelSummary(KeyframeAttributes Type, int Count, uint FirstFrame, uint LastFrame);

    private const ushort KeyframePosition = 1 << 0;
    private const ushort KeyframeEulerRotation = 1 << 1;
    private const ushort KeyframeScale = 1 << 2;
    private const ushort KeyframeVector = 1 << 3;
    private const ushort KeyframeVertex = 1 << 4;
    private const ushort KeyframeNormal = 1 << 5;
    private const ushort KeyframeTarget = 1 << 6;
    private const ushort KeyframeRoll = 1 << 7;
    private const ushort KeyframeAngle = 1 << 8;
    private const ushort KeyframeLightColor = 1 << 9;
    private const ushort KeyframeIntensity = 1 << 10;
    private const ushort KeyframeSpot = 1 << 11;
    private const ushort KeyframePoint = 1 << 12;
    private const ushort KeyframeQuaternionRotation = 1 << 13;

    private static readonly ushort[] KeyframeAttributeOrder =
    [
        KeyframePosition,
        KeyframeEulerRotation,
        KeyframeScale,
        KeyframeVector,
        KeyframeVertex,
        KeyframeNormal,
        KeyframeTarget,
        KeyframeRoll,
        KeyframeAngle,
        KeyframeLightColor,
        KeyframeIntensity,
        KeyframeSpot,
        KeyframePoint,
        KeyframeQuaternionRotation,
    ];

    private sealed class SafeMotionSummary
    {
        public uint NodeCount { get; set; }
        public uint InterpolationMode { get; set; }
        public bool ShortRot { get; set; }
        public ushort ManualKeyframeTypes { get; set; }
        public ushort KeyframeTypes { get; set; }
        public uint FrameCount { get; set; }
        public List<SafeKeyframesSummary> Keyframes { get; } = [];
    }

    private readonly record struct SafeKeyframesSummary(
        int NodeIndex,
        ushort Type,
        uint KeyframeCount,
        List<SafeKeyframeChannelSummary> Channels);

    private readonly record struct SafeKeyframeChannelSummary(
        ushort Type,
        uint Count,
        uint FirstFrame,
        uint LastFrame);

    private struct Slice9NormalizationSummary
    {
        public int AttachCount { get; set; }
        public int BufferMeshCount { get; set; }
        public int BufferVertexCount { get; set; }
        public int BufferCornerCount { get; set; }
        public int BufferTriangleCornerCount { get; set; }
        public int WeightedMeshCount { get; set; }
        public int WeightedVertexCount { get; set; }
        public int WeightedTriangleSetCount { get; set; }
        public int WeightedTriangleCornerCount { get; set; }
    }

    private struct Slice9WeightedSummary
    {
        public int VertexCount { get; set; }
        public int TriangleSetCount { get; set; }
        public int TriangleCornerCount { get; set; }
    }

    private sealed class StagedSa3dSummary
    {
        public int Slice3ModelBlockCount { get; set; }
        public int Slice3ParsedModelCount { get; set; }
        public int Slice3NodeCount { get; set; }
        public int Slice3AttachRefCount { get; set; }
        public int Slice3GraphErrorCount { get; set; }
        public string Slice3StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);

        public int Slice4ChunkAttachCount { get; set; }
        public int Slice4VertexChunkCount { get; set; }
        public int Slice4VertexCount { get; set; }
        public int Slice4WeightedVertexChunkCount { get; set; }
        public string Slice4StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);

        public int Slice5PolyChunkCount { get; set; }
        public int Slice5NullPolyChunkCount { get; set; }
        public int Slice5BitsChunkCount { get; set; }
        public int Slice5TextureChunkCount { get; set; }
        public int Slice5MaterialChunkCount { get; set; }
        public int Slice5MaterialBumpChunkCount { get; set; }
        public int Slice5StripChunkCount { get; set; }
        public int Slice5PolyCornerCount { get; set; }
        public string Slice5StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);
        public string Slice5TypeHash { get; set; } = ToHex64(FnvOffsetBasis);
        public string Slice5AttributeHash { get; set; } = ToHex64(FnvOffsetBasis);
        public string Slice5ByteSizeHash { get; set; } = ToHex64(FnvOffsetBasis);
        public string Slice5StripMetaHash { get; set; } = ToHex64(FnvOffsetBasis);
        public int Slice6ModelFileCheckCount { get; set; }
        public int Slice6ParsedModelFileCount { get; set; }
        public int Slice6NodeCount { get; set; }
        public int Slice6AttachRefCount { get; set; }
        public int Slice6ChunkAttachCount { get; set; }
        public int Slice6PolyChunkCount { get; set; }
        public string Slice6StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);
        public int Slice7MotionBlockCount { get; set; }
        public int Slice7ParsedMotionCount { get; set; }
        public int Slice7NodeCount { get; set; }
        public int Slice7KeyframeSetCount { get; set; }
        public int Slice7ChannelCount { get; set; }
        public int Slice7KeyframeCount { get; set; }
        public string Slice7StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);
        public int Slice8AnimationFileCheckCount { get; set; }
        public int Slice8ParsedAnimationFileCount { get; set; }
        public int Slice8NodeCount { get; set; }
        public int Slice8KeyframeSetCount { get; set; }
        public int Slice8ChannelCount { get; set; }
        public int Slice8KeyframeCount { get; set; }
        public string Slice8StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);
        public int Slice9AttachCount { get; set; }
        public int Slice9BufferMeshCount { get; set; }
        public int Slice9BufferVertexCount { get; set; }
        public int Slice9BufferCornerCount { get; set; }
        public int Slice9BufferTriangleCornerCount { get; set; }
        public int Slice9WeightedMeshCount { get; set; }
        public int Slice9WeightedVertexCount { get; set; }
        public int Slice9WeightedTriangleSetCount { get; set; }
        public int Slice9WeightedTriangleCornerCount { get; set; }
        public string Slice9StructuralHash { get; set; } = ToHex64(FnvOffsetBasis);

        public int DiagnosticCount { get; set; }

        public SortedDictionary<string, JsonElement> ToJsonDictionary()
        {
            return new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["slice3_model_block_count"] = JsonSerializer.SerializeToElement(Slice3ModelBlockCount),
                ["slice3_parsed_model_count"] = JsonSerializer.SerializeToElement(Slice3ParsedModelCount),
                ["slice3_node_count"] = JsonSerializer.SerializeToElement(Slice3NodeCount),
                ["slice3_attach_ref_count"] = JsonSerializer.SerializeToElement(Slice3AttachRefCount),
                ["slice3_graph_error_count"] = JsonSerializer.SerializeToElement(Slice3GraphErrorCount),
                ["slice3_structural_hash"] = JsonSerializer.SerializeToElement(Slice3StructuralHash),
                ["slice4_chunk_attach_count"] = JsonSerializer.SerializeToElement(Slice4ChunkAttachCount),
                ["slice4_vertex_chunk_count"] = JsonSerializer.SerializeToElement(Slice4VertexChunkCount),
                ["slice4_vertex_count"] = JsonSerializer.SerializeToElement(Slice4VertexCount),
                ["slice4_weighted_vertex_chunk_count"] = JsonSerializer.SerializeToElement(Slice4WeightedVertexChunkCount),
                ["slice4_structural_hash"] = JsonSerializer.SerializeToElement(Slice4StructuralHash),
                ["slice5_poly_chunk_count"] = JsonSerializer.SerializeToElement(Slice5PolyChunkCount),
                ["slice5_null_poly_chunk_count"] = JsonSerializer.SerializeToElement(Slice5NullPolyChunkCount),
                ["slice5_bits_chunk_count"] = JsonSerializer.SerializeToElement(Slice5BitsChunkCount),
                ["slice5_texture_chunk_count"] = JsonSerializer.SerializeToElement(Slice5TextureChunkCount),
                ["slice5_material_chunk_count"] = JsonSerializer.SerializeToElement(Slice5MaterialChunkCount),
                ["slice5_material_bump_chunk_count"] = JsonSerializer.SerializeToElement(Slice5MaterialBumpChunkCount),
                ["slice5_strip_chunk_count"] = JsonSerializer.SerializeToElement(Slice5StripChunkCount),
                ["slice5_poly_corner_count"] = JsonSerializer.SerializeToElement(Slice5PolyCornerCount),
                ["slice5_structural_hash"] = JsonSerializer.SerializeToElement(Slice5StructuralHash),
                ["slice5_type_hash"] = JsonSerializer.SerializeToElement(Slice5TypeHash),
                ["slice5_attribute_hash"] = JsonSerializer.SerializeToElement(Slice5AttributeHash),
                ["slice5_byte_size_hash"] = JsonSerializer.SerializeToElement(Slice5ByteSizeHash),
                ["slice5_strip_meta_hash"] = JsonSerializer.SerializeToElement(Slice5StripMetaHash),
                ["slice6_model_file_check_count"] = JsonSerializer.SerializeToElement(Slice6ModelFileCheckCount),
                ["slice6_parsed_model_file_count"] = JsonSerializer.SerializeToElement(Slice6ParsedModelFileCount),
                ["slice6_node_count"] = JsonSerializer.SerializeToElement(Slice6NodeCount),
                ["slice6_attach_ref_count"] = JsonSerializer.SerializeToElement(Slice6AttachRefCount),
                ["slice6_chunk_attach_count"] = JsonSerializer.SerializeToElement(Slice6ChunkAttachCount),
                ["slice6_poly_chunk_count"] = JsonSerializer.SerializeToElement(Slice6PolyChunkCount),
                ["slice6_structural_hash"] = JsonSerializer.SerializeToElement(Slice6StructuralHash),
                ["slice7_motion_block_count"] = JsonSerializer.SerializeToElement(Slice7MotionBlockCount),
                ["slice7_parsed_motion_count"] = JsonSerializer.SerializeToElement(Slice7ParsedMotionCount),
                ["slice7_node_count"] = JsonSerializer.SerializeToElement(Slice7NodeCount),
                ["slice7_keyframe_set_count"] = JsonSerializer.SerializeToElement(Slice7KeyframeSetCount),
                ["slice7_channel_count"] = JsonSerializer.SerializeToElement(Slice7ChannelCount),
                ["slice7_keyframe_count"] = JsonSerializer.SerializeToElement(Slice7KeyframeCount),
                ["slice7_structural_hash"] = JsonSerializer.SerializeToElement(Slice7StructuralHash),
                ["slice8_animation_file_check_count"] = JsonSerializer.SerializeToElement(Slice8AnimationFileCheckCount),
                ["slice8_parsed_animation_file_count"] = JsonSerializer.SerializeToElement(Slice8ParsedAnimationFileCount),
                ["slice8_node_count"] = JsonSerializer.SerializeToElement(Slice8NodeCount),
                ["slice8_keyframe_set_count"] = JsonSerializer.SerializeToElement(Slice8KeyframeSetCount),
                ["slice8_channel_count"] = JsonSerializer.SerializeToElement(Slice8ChannelCount),
                ["slice8_keyframe_count"] = JsonSerializer.SerializeToElement(Slice8KeyframeCount),
                ["slice8_structural_hash"] = JsonSerializer.SerializeToElement(Slice8StructuralHash),
                ["slice9_attach_count"] = JsonSerializer.SerializeToElement(Slice9AttachCount),
                ["slice9_buffer_mesh_count"] = JsonSerializer.SerializeToElement(Slice9BufferMeshCount),
                ["slice9_buffer_vertex_count"] = JsonSerializer.SerializeToElement(Slice9BufferVertexCount),
                ["slice9_buffer_corner_count"] = JsonSerializer.SerializeToElement(Slice9BufferCornerCount),
                ["slice9_buffer_triangle_corner_count"] = JsonSerializer.SerializeToElement(Slice9BufferTriangleCornerCount),
                ["slice9_weighted_mesh_count"] = JsonSerializer.SerializeToElement(Slice9WeightedMeshCount),
                ["slice9_weighted_vertex_count"] = JsonSerializer.SerializeToElement(Slice9WeightedVertexCount),
                ["slice9_weighted_triangle_set_count"] = JsonSerializer.SerializeToElement(Slice9WeightedTriangleSetCount),
                ["slice9_weighted_triangle_corner_count"] = JsonSerializer.SerializeToElement(Slice9WeightedTriangleCornerCount),
                ["slice9_structural_hash"] = JsonSerializer.SerializeToElement(Slice9StructuralHash),
            };
        }
    }

    private static string ComputeJsonHash(JsonElement value)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(value, JsonOptions);
        return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }

    private const ulong FnvOffsetBasis = 14695981039346656037UL;
    private const ulong FnvPrime = 1099511628211UL;

    private readonly record struct ScannedNjBlock(uint Offset, uint Header, uint Size, string Role);

    private static IReadOnlyList<ScannedNjBlock> ScanNjBlocks(byte[] bytes)
    {
        var result = new List<ScannedNjBlock>();
        if (bytes.Length < 8)
        {
            return result;
        }

        var sizeBigEndian = CheckBigEndian32(bytes, 4);
        uint blockAddress = 0;
        while ((ulong)blockAddress < (ulong)bytes.Length + 8UL)
        {
            if ((ulong)blockAddress + 8UL > (ulong)bytes.Length)
            {
                break;
            }

            var header = ReadU32LittleEndian(bytes, checked((int)blockAddress));
            var size = sizeBigEndian
                ? ReadU32BigEndian(bytes, checked((int)blockAddress + 4))
                : ReadU32LittleEndian(bytes, checked((int)blockAddress + 4));
            if (header == 0 || size == 0)
            {
                break;
            }

            result.Add(new(blockAddress, header, size, GetSkiesNjRole(header)));
            blockAddress += 8 + size;
        }

        return result;
    }

    private static bool CheckBigEndian32(byte[] bytes, int offset)
    {
        if (offset < 0 || offset + 4 > bytes.Length)
        {
            return false;
        }

        var little = ReadU32LittleEndian(bytes, offset);
        var big = ReadU32BigEndian(bytes, offset);
        var remaining = bytes.Length - offset;
        var littlePlausible = little > 0 && little <= remaining;
        var bigPlausible = big > 0 && big <= remaining;
        return bigPlausible && !littlePlausible;
    }

    private static string GetSkiesNjRole(uint header)
    {
        return header switch
        {
            0x4D434A4E => "model",
            0x4D424A4E => "model",
            0x4C544A4E => "texture",
            0x4D444D4E => "animation",
            0x4D53534E => "animation",
            0x4D41434E => "animation",
            _ => "none",
        };
    }

    private static void FnvUpdateByte(ref ulong hash, byte value)
    {
        hash ^= value;
        hash *= FnvPrime;
    }

    private static void FnvUpdateUInt32(ref ulong hash, uint value)
    {
        for (var i = 0; i < 4; i++)
        {
            FnvUpdateByte(ref hash, (byte)((value >> (i * 8)) & 0xFF));
        }
    }

    private static void FnvUpdateString(ref ulong hash, string value)
    {
        foreach (var c in Encoding.ASCII.GetBytes(value))
        {
            FnvUpdateByte(ref hash, c);
        }
        FnvUpdateByte(ref hash, 0);
    }

    private static string ToHex64(ulong value)
    {
        return value.ToString("x16", CultureInfo.InvariantCulture);
    }

    private static bool TryInvokeParityReportGenerator(
        System.Reflection.Assembly assembly,
        string inputPath,
        int requestedSlice,
        BlockManifest blockManifest,
        List<Diagnostic> diagnostics,
        out ParserInvocationResult result)
    {
        result = ParserInvocationResult.Failed("parity_invoke_failed");
        var generatorType = assembly.GetType("SA3D.Modeling.Parity.ParityReportGenerator", throwOnError: false);
        var optionsType = assembly.GetType("SA3D.Modeling.Parity.ParityCaptureOptions", throwOnError: false);
        if (generatorType is null || optionsType is null)
        {
            return false;
        }

        var createMethod = generatorType
            .GetMethods(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)
            .FirstOrDefault(method =>
            {
                if (!method.Name.Equals("CreateFromBytes", StringComparison.Ordinal))
                {
                    return false;
                }
                var parameters = method.GetParameters();
                return parameters.Length > 0 && parameters[0].ParameterType == typeof(byte[]);
            });
        if (createMethod is null)
        {
            return false;
        }

        var parsedObject = 0;
        var parsedMotion = 0;
        var failedBlocks = 0;
        var parityErrorBlocks = 0;
        var firstModelOffset = blockManifest.Blocks
            .Where(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase))
            .Select(x => (int?)x.Offset)
            .FirstOrDefault();
        var firstMotionOffset = blockManifest.Blocks
            .Where(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
            .Select(x => (int?)x.Offset)
            .FirstOrDefault();
        var collatedFunctionPairs = new List<SliceFunctionPair>();
        var fixtureId = Path.GetFileNameWithoutExtension(inputPath);

        foreach (var block in blockManifest.Blocks.OrderBy(x => x.Index))
        {
            if (string.IsNullOrWhiteSpace(block.Path) || !File.Exists(block.Path))
            {
                failedBlocks++;
                diagnostics.Add(new Diagnostic
                {
                    Code = "SA3D_BLOCK_FILE_MISSING",
                    Severity = "error",
                    Stage = "reference_parse",
                    Message = $"Missing block file for index={block.Index} kind={block.Kind}.",
                });
                continue;
            }

            var blockBytes = File.ReadAllBytes(block.Path);
            try
            {
                var optionsInstance = BuildParityCaptureOptions(optionsType, requestedSlice, block.Offset, block.Kind);
                var reportObject = createMethod.Invoke(null, BuildParityCreateArgs(createMethod, blockBytes, optionsInstance, fixtureId, block));
                if (reportObject is null)
                {
                    failedBlocks++;
                    diagnostics.Add(new Diagnostic
                    {
                        Code = "SA3D_PARITY_REPORT_NULL",
                        Severity = "error",
                        Stage = "reference_parse",
                        Message = $"Parity report was null for block index={block.Index} kind={block.Kind}.",
                    });
                    continue;
                }

                var reportJson = JsonSerializer.SerializeToElement(reportObject, reportObject.GetType(), JsonOptions);
                if (block.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
                {
                    parsedMotion++;
                }
                else
                {
                    parsedObject++;
                }

                if (AppendParityDiagnostics(diagnostics, reportJson, block))
                {
                    parityErrorBlocks++;
                }

                var blockPairs = ExtractParityFunctionPairs(reportJson, requestedSlice, block, blockBytes);
                if (blockPairs.Count == 0)
                {
                    diagnostics.Add(new Diagnostic
                    {
                        Code = "SA3D_PARITY_SLICE_MISSING",
                        Severity = "warning",
                        Stage = "reference_parse",
                        Message = $"No slice_io_pairs for requested slice={requestedSlice} on block index={block.Index} kind={block.Kind}.",
                    });
                }
                collatedFunctionPairs.AddRange(blockPairs);
            }
            catch (Exception ex)
            {
                failedBlocks++;
                diagnostics.Add(new Diagnostic
                {
                    Code = "SA3D_BLOCK_PARSE_FAILED",
                    Severity = "error",
                    Stage = "reference_parse",
                    Message = $"Failed to parse block index={block.Index} kind={block.Kind}: {ex.GetBaseException().Message}",
                });
            }
        }

        var structural = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("sa3d_modeling_parity_report_generator"),
            ["parsed_object_blocks"] = JsonSerializer.SerializeToElement(parsedObject),
            ["parsed_motion_blocks"] = JsonSerializer.SerializeToElement(parsedMotion),
            ["failed_blocks"] = JsonSerializer.SerializeToElement(failedBlocks),
            ["parity_error_blocks"] = JsonSerializer.SerializeToElement(parityErrorBlocks),
            ["collated_slice_pairs"] = JsonSerializer.SerializeToElement(collatedFunctionPairs.Count),
        };

        var semantic = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["reference_library"] = JsonSerializer.SerializeToElement("SA3D.Modeling"),
            ["capture_mode"] = JsonSerializer.SerializeToElement("parity_report_generator"),
        };

        var outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("sa3d_modeling_parity_report_generator"),
            ["capture_mode"] = JsonSerializer.SerializeToElement("parity_report_generator"),
            ["parsed_object_blocks"] = JsonSerializer.SerializeToElement(parsedObject),
            ["parsed_motion_blocks"] = JsonSerializer.SerializeToElement(parsedMotion),
            ["failed_blocks"] = JsonSerializer.SerializeToElement(failedBlocks),
            ["parity_error_blocks"] = JsonSerializer.SerializeToElement(parityErrorBlocks),
            ["collated_slice_pairs"] = JsonSerializer.SerializeToElement(collatedFunctionPairs.Count),
            ["slice"] = JsonSerializer.SerializeToElement(requestedSlice),
            ["fixture_input_blob_base64"] = JsonSerializer.SerializeToElement(Convert.ToBase64String(File.ReadAllBytes(inputPath))),
        };

        result = new ParserInvocationResult
        {
            Invoked = true,
            Status = failedBlocks == 0 && parityErrorBlocks == 0 ? "ok" : "partial",
            Structural = structural,
            Semantic = semantic,
            Outputs = outputs,
            ModelBlockOffset = firstModelOffset,
            MotionBlockOffset = firstMotionOffset,
            CollatedSlicePairs = GroupFunctionPairsBySlice(collatedFunctionPairs),
        };
        return true;
    }

    private static ParserInvocationResult InvokeLegacyReadFromBytes(
        System.Reflection.Assembly assembly,
        string inputPath,
        int slice,
        BlockManifest blockManifest,
        List<Diagnostic> diagnostics,
        string assemblyPath)
    {
        var modelType = assembly.GetType("SA3D.Modeling.File.ModelFile", throwOnError: false);
        var animationType = assembly.GetType("SA3D.Modeling.File.AnimationFile", throwOnError: false);
        if (modelType is null || animationType is null)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "SA3D_TYPES_NOT_FOUND",
                Severity = "error",
                Stage = "reference_invoke",
                Message = "Loaded SA3D.Modeling assembly but required file wrapper types were not found.",
            });
            return ParserInvocationResult.Failed("types_not_found");
        }

        var modelReadMethod = FindReadFromBytesMethod(modelType);
        var motionReadMethod = FindReadFromBytesMethod(animationType);
        if (modelReadMethod is null || motionReadMethod is null)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "SA3D_READ_METHOD_NOT_FOUND",
                Severity = "error",
                Stage = "reference_invoke",
                Message = "Could not locate compatible ReadFromBytes APIs on SA3D.Modeling file wrappers.",
            });
            return ParserInvocationResult.Failed("read_api_missing");
        }

        var parsedObject = 0;
        var parsedMotion = 0;
        var failedBlocks = 0;
        var firstModelOffset = blockManifest.Blocks
            .Where(x => x.Kind.Equals("object", StringComparison.OrdinalIgnoreCase))
            .Select(x => (int?)x.Offset)
            .FirstOrDefault();
        var firstMotionOffset = blockManifest.Blocks
            .Where(x => x.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
            .Select(x => (int?)x.Offset)
            .FirstOrDefault();

        foreach (var block in blockManifest.Blocks)
        {
            if (string.IsNullOrWhiteSpace(block.Path) || !File.Exists(block.Path))
            {
                failedBlocks++;
                continue;
            }

            var blockBytes = File.ReadAllBytes(block.Path);
            var targetMethod = block.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase)
                ? motionReadMethod
                : modelReadMethod;
            try
            {
                _ = targetMethod.Invoke(null, BuildReadFromBytesArgs(targetMethod, blockBytes));
                if (block.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase))
                {
                    parsedMotion++;
                }
                else
                {
                    parsedObject++;
                }
            }
            catch (Exception ex)
            {
                failedBlocks++;
                diagnostics.Add(new Diagnostic
                {
                    Code = "SA3D_BLOCK_PARSE_FAILED",
                    Severity = "error",
                    Stage = "reference_parse",
                    Message = $"Failed to parse block index={block.Index} kind={block.Kind}: {ex.GetBaseException().Message}",
                });
            }
        }

        var structural = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["sa3d_modeling_assembly"] = JsonSerializer.SerializeToElement(assemblyPath),
            ["parsed_object_blocks"] = JsonSerializer.SerializeToElement(parsedObject),
            ["parsed_motion_blocks"] = JsonSerializer.SerializeToElement(parsedMotion),
            ["failed_blocks"] = JsonSerializer.SerializeToElement(failedBlocks),
        };

        var semantic = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["reference_library"] = JsonSerializer.SerializeToElement("SA3D.Modeling"),
            ["capture_mode"] = JsonSerializer.SerializeToElement("legacy_readfrombytes"),
        };

        var outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["parser_binding"] = JsonSerializer.SerializeToElement("sa3d_modeling_reflection"),
            ["capture_mode"] = JsonSerializer.SerializeToElement("legacy_readfrombytes"),
            ["sa3d_modeling_dll"] = JsonSerializer.SerializeToElement(assemblyPath),
            ["parsed_object_blocks"] = JsonSerializer.SerializeToElement(parsedObject),
            ["parsed_motion_blocks"] = JsonSerializer.SerializeToElement(parsedMotion),
            ["failed_blocks"] = JsonSerializer.SerializeToElement(failedBlocks),
            ["slice"] = JsonSerializer.SerializeToElement(slice),
            ["fixture_input_blob_base64"] = JsonSerializer.SerializeToElement(Convert.ToBase64String(File.ReadAllBytes(inputPath))),
        };

        return new ParserInvocationResult
        {
            Invoked = true,
            Status = failedBlocks == 0 ? "ok" : "partial",
            Structural = structural,
            Semantic = semantic,
            Outputs = outputs,
            ModelBlockOffset = firstModelOffset,
            MotionBlockOffset = firstMotionOffset,
        };
    }

    private static bool TryResolveSa3dModelingAssemblyPath(Dictionary<string, string> options, out string assemblyPath)
    {
        var candidates = new List<string>();
        if (options.TryGetValue("--sa3d-modeling-dll", out var explicitPath) && !string.IsNullOrWhiteSpace(explicitPath))
        {
            candidates.Add(Path.GetFullPath(explicitPath));
        }

        var appBase = AppContext.BaseDirectory;
        candidates.Add(Path.Combine(appBase, "SA3D.Modeling.dll"));
        candidates.Add(Path.GetFullPath(Path.Combine(appBase, "..", "..", "..", "..", "third-party", "SA3D.Modeling", "SA3D.Modeling", "bin", "Debug", "net8.0", "SA3D.Modeling.dll")));
        candidates.Add(Path.GetFullPath(Path.Combine(appBase, "..", "..", "..", "..", "third-party", "SA3D.Modeling", "SA3D.Modeling", "bin", "Release", "net8.0", "SA3D.Modeling.dll")));

        assemblyPath = candidates.FirstOrDefault(File.Exists) ?? string.Empty;
        return !string.IsNullOrWhiteSpace(assemblyPath);
    }

    private static System.Reflection.MethodInfo? FindReadFromBytesMethod(Type wrapperType)
    {
        return wrapperType
            .GetMethods(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)
            .FirstOrDefault(method =>
            {
                if (!method.Name.Equals("ReadFromBytes", StringComparison.Ordinal))
                {
                    return false;
                }
                var parameters = method.GetParameters();
                return parameters.Length > 0 && parameters[0].ParameterType == typeof(byte[]);
            });
    }

    private static object?[] BuildReadFromBytesArgs(System.Reflection.MethodInfo method, byte[] bytes)
    {
        var parameters = method.GetParameters();
        var args = new object?[parameters.Length];
        for (var i = 0; i < parameters.Length; i++)
        {
            if (i == 0)
            {
                args[i] = bytes;
                continue;
            }
            args[i] = parameters[i].HasDefaultValue ? parameters[i].DefaultValue : GetDefault(parameters[i].ParameterType);
        }
        return args;
    }

    private static object? GetDefault(Type type)
    {
        return type.IsValueType ? Activator.CreateInstance(type) : null;
    }

    private static object? BuildParityCaptureOptions(Type optionsType, int requestedSlice, int blockOffset, string blockKind)
    {
        var instance = Activator.CreateInstance(optionsType);
        if (instance is null)
        {
            return null;
        }

        SetPropertyIfPresent(optionsType, instance, "EnableCapture", true);
        SetPropertyIfPresent(optionsType, instance, "CaptureEnabled", true);
        SetPropertyIfPresent(optionsType, instance, "Address", 0);
        SetPropertyIfPresent(optionsType, instance, "ImageBase", blockOffset);
        SetPropertyIfPresent(optionsType, instance, "IsAnimation", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase));
        SetPropertyIfPresent(optionsType, instance, "TreatAsAnimation", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase));
        SetPropertyIfPresent(optionsType, instance, "TryAnimationFallback", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase));
        SetEnumPropertyIfPresent(optionsType, instance, "ParseAdapter", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase) ? "AnimationFile" : "ModelFile");

        var requestedSlicesProperty = optionsType.GetProperty("RequestedSlices", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance);
        if (requestedSlicesProperty is not null
            && requestedSlicesProperty.CanWrite
            && requestedSlicesProperty.PropertyType.IsAssignableFrom(typeof(int[])))
        {
            requestedSlicesProperty.SetValue(instance, new[] { requestedSlice });
        }

        return instance;
    }

    private static void SetPropertyIfPresent(Type type, object target, string propertyName, object value)
    {
        var property = type.GetProperty(propertyName, System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        if (property is null || !property.CanWrite)
        {
            return;
        }

        try
        {
            var normalized = ConvertValueForType(value, property.PropertyType);
            property.SetValue(target, normalized);
        }
        catch
        {
            // intentionally swallow: we support a best-effort reflection bridge across branch variants.
        }
    }

    private static void SetEnumPropertyIfPresent(Type type, object target, string propertyName, string enumValueName)
    {
        var property = type.GetProperty(propertyName, System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        if (property is null || !property.CanWrite || !property.PropertyType.IsEnum)
        {
            return;
        }

        try
        {
            var value = Enum.Parse(property.PropertyType, enumValueName, ignoreCase: true);
            property.SetValue(target, value);
        }
        catch
        {
            // best-effort across parity API variants
        }
    }

    private static object? ConvertValueForType(object value, Type targetType)
    {
        if (value is null)
        {
            return null;
        }

        var effectiveType = Nullable.GetUnderlyingType(targetType) ?? targetType;
        if (effectiveType.IsAssignableFrom(value.GetType()))
        {
            return value;
        }

        if (effectiveType == typeof(int))
        {
            return Convert.ToInt32(value, CultureInfo.InvariantCulture);
        }
        if (effectiveType == typeof(uint))
        {
            return Convert.ToUInt32(value, CultureInfo.InvariantCulture);
        }
        if (effectiveType == typeof(bool))
        {
            return Convert.ToBoolean(value, CultureInfo.InvariantCulture);
        }
        if (effectiveType == typeof(string))
        {
            return Convert.ToString(value, CultureInfo.InvariantCulture);
        }

        return value;
    }

    private static object?[] BuildParityCreateArgs(
        System.Reflection.MethodInfo method,
        byte[] bytes,
        object? optionsInstance,
        string fixtureId,
        BlockManifestItem block)
    {
        var parameters = method.GetParameters();
        var args = new object?[parameters.Length];
        for (var i = 0; i < parameters.Length; i++)
        {
            var parameter = parameters[i];
            if (i == 0 && parameter.ParameterType == typeof(byte[]))
            {
                args[i] = bytes;
                continue;
            }

            if (optionsInstance is not null && parameter.ParameterType.IsInstanceOfType(optionsInstance))
            {
                args[i] = optionsInstance;
                continue;
            }

            if (parameter.ParameterType == typeof(string))
            {
                if (parameter.Name?.Contains("fixture", StringComparison.OrdinalIgnoreCase) == true)
                {
                    args[i] = fixtureId;
                }
                else if (parameter.Name?.Contains("run", StringComparison.OrdinalIgnoreCase) == true)
                {
                    args[i] = $"{fixtureId}-block-{block.Index}";
                }
                else if (parameter.HasDefaultValue)
                {
                    args[i] = parameter.DefaultValue;
                }
                else
                {
                    args[i] = string.Empty;
                }
                continue;
            }

            if (parameter.ParameterType == typeof(bool)
                && parameter.Name?.Contains("animation", StringComparison.OrdinalIgnoreCase) == true)
            {
                args[i] = block.Kind.Equals("motion", StringComparison.OrdinalIgnoreCase);
                continue;
            }

            if (parameter.HasDefaultValue)
            {
                args[i] = parameter.DefaultValue;
            }
            else
            {
                args[i] = GetDefault(parameter.ParameterType);
            }
        }
        return args;
    }

    private static bool AppendParityDiagnostics(List<Diagnostic> diagnostics, JsonElement reportJson, BlockManifestItem block)
    {
        if (!TryGetProperty(reportJson, out var diagnosticsElement, "diagnostics", "Diagnostics")
            || diagnosticsElement.ValueKind != JsonValueKind.Array)
        {
            return false;
        }

        var hasError = false;
        foreach (var item in diagnosticsElement.EnumerateArray())
        {
            var code = TryGetString(item, "code", "Code");
            var message = TryGetString(item, "message", "Message");
            var stage = TryGetString(item, "stage", "Stage");
            var severity = TryGetString(item, "severity", "Severity");
            var normalizedSeverity = string.IsNullOrWhiteSpace(severity) ? "warning" : severity;
            if (normalizedSeverity.Equals("error", StringComparison.OrdinalIgnoreCase))
            {
                hasError = true;
            }

            diagnostics.Add(new Diagnostic
            {
                Code = string.IsNullOrWhiteSpace(code) ? "SA3D_PARITY_DIAGNOSTIC" : code,
                Severity = normalizedSeverity,
                Stage = string.IsNullOrWhiteSpace(stage) ? "reference_parse" : stage,
                Message = $"block index={block.Index} kind={block.Kind}: {message}",
            });
        }
        return hasError;
    }

    private static List<SliceFunctionPair> ExtractParityFunctionPairs(
        JsonElement reportJson,
        int requestedSlice,
        BlockManifestItem block,
        byte[] blockBytes)
    {
        var pairs = new List<SliceFunctionPair>();
        if (!TryGetProperty(reportJson, out var slicePairsElement, "slice_io_pairs", "SliceIOPairs")
            || slicePairsElement.ValueKind != JsonValueKind.Array)
        {
            return pairs;
        }

        var sequence = 0;
        var blockBlobBase64 = Convert.ToBase64String(blockBytes);
        foreach (var pairElement in slicePairsElement.EnumerateArray())
        {
            var pairSlice = TryGetInt(pairElement, "slice", "Slice");
            if (pairSlice.HasValue && pairSlice.Value != requestedSlice)
            {
                continue;
            }

            var inputs = TryGetProperty(pairElement, out var inputElement, "inputs", "Inputs")
                ? ConvertObjectToSortedDictionary(inputElement)
                : new SortedDictionary<string, JsonElement>(StringComparer.Ordinal);
            var outputs = TryGetProperty(pairElement, out var outputElement, "outputs", "Outputs")
                ? ConvertObjectToSortedDictionary(outputElement)
                : new SortedDictionary<string, JsonElement>(StringComparer.Ordinal);

            inputs["block_index"] = JsonSerializer.SerializeToElement(block.Index);
            inputs["block_kind"] = JsonSerializer.SerializeToElement(block.Kind);
            inputs["block_offset"] = JsonSerializer.SerializeToElement(block.Offset);
            inputs["block_size"] = JsonSerializer.SerializeToElement(block.Size);
            inputs["pair_sequence_in_block"] = JsonSerializer.SerializeToElement(sequence);
            inputs["input_blob_base64"] = JsonSerializer.SerializeToElement(blockBlobBase64);
            inputs["input_blob_encoding"] = JsonSerializer.SerializeToElement("base64");

            var operationRoutes = BuildOperationRoutes(inputElement, outputElement, block.Kind);
            foreach (var route in operationRoutes)
            {
                var pairInputs = new SortedDictionary<string, JsonElement>(inputs, StringComparer.Ordinal)
                {
                    ["operation"] = JsonSerializer.SerializeToElement(route.Operation),
                };

                pairs.Add(new SliceFunctionPair
                {
                    Slice = pairSlice ?? requestedSlice,
                    Pair = new FunctionIoPair
                    {
                        FunctionId = route.FunctionId,
                        InputFields = pairInputs,
                        Output = JsonSerializer.SerializeToElement(outputs, JsonOptions),
                    },
                });
            }

            sequence++;
        }
        return pairs;
    }

    private static List<SliceIoPair> GroupFunctionPairsBySlice(List<SliceFunctionPair> functionPairs)
    {
        return functionPairs
            .GroupBy(x => x.Slice)
            .OrderBy(group => group.Key)
            .Select(group => new SliceIoPair
            {
                Slice = group.Key,
                Pairs = group
                    .Select(x => x.Pair)
                    .OrderBy(x => x.FunctionId, StringComparer.Ordinal)
                    .ToList(),
            })
            .ToList();
    }

    private static List<OperationRoute> BuildOperationRoutes(JsonElement inputs, JsonElement outputs, string blockKind)
    {
        var operations = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        CollectOperationNames(inputs, operations);
        CollectOperationNames(outputs, operations);
        if (operations.Count == 0)
        {
            operations.Add(blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase) ? "motion_decode" : "model_decode");
        }

        return operations
            .OrderBy(x => x, StringComparer.OrdinalIgnoreCase)
            .Select(operation => new OperationRoute
            {
                Operation = operation,
                FunctionId = ResolveFunctionId(operation),
            })
            .ToList();
    }

    private static void CollectOperationNames(JsonElement element, HashSet<string> operations)
    {
        switch (element.ValueKind)
        {
            case JsonValueKind.Object:
                foreach (var property in element.EnumerateObject())
                {
                    if (property.NameEquals("operation") || property.NameEquals("operation_kind") || property.NameEquals("Operation") || property.NameEquals("OperationKind"))
                    {
                        var op = property.Value.ValueKind == JsonValueKind.String ? property.Value.GetString() : null;
                        if (!string.IsNullOrWhiteSpace(op))
                        {
                            operations.Add(op);
                        }
                    }
                    CollectOperationNames(property.Value, operations);
                }
                break;
            case JsonValueKind.Array:
                foreach (var item in element.EnumerateArray())
                {
                    CollectOperationNames(item, operations);
                }
                break;
        }
    }

    private static string ResolveFunctionId(string operation)
    {
        return operation.ToLowerInvariant() switch
        {
            "primitive_op" => "Sa3Dport.Testing.Slice1TestApi.PrimitiveOp",
            "bams_checkpoint" => "Sa3Dport.Testing.Slice1TestApi.BamsCheckpoint",
            "lut_op" => "Sa3Dport.Testing.Slice1TestApi.LutOp",
            "nj_blocks" => "Sa3Dport.Testing.Slice2TestApi.NjBlocks",
            "motion_decode" => "Sa3Dport.File.AnimationFile.ReadNJ",
            "model_decode" => "Sa3Dport.File.ModelFile.ReadNJ",
            _ => $"unknown:{operation}",
        };
    }

    private static bool TryGetProperty(JsonElement element, out JsonElement value, params string[] names)
    {
        foreach (var name in names)
        {
            if (element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out value))
            {
                return true;
            }
        }

        value = default;
        return false;
    }

    private static string TryGetString(JsonElement element, params string[] names)
    {
        if (!TryGetProperty(element, out var value, names) || value.ValueKind != JsonValueKind.String)
        {
            return string.Empty;
        }
        return value.GetString() ?? string.Empty;
    }

    private static int? TryGetInt(JsonElement element, params string[] names)
    {
        if (!TryGetProperty(element, out var value, names))
        {
            return null;
        }
        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var intValue))
        {
            return intValue;
        }
        return null;
    }

    private static SortedDictionary<string, JsonElement> ConvertObjectToSortedDictionary(JsonElement element)
    {
        var result = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal);
        if (element.ValueKind != JsonValueKind.Object)
        {
            result["value"] = element;
            return result;
        }

        foreach (var property in element.EnumerateObject().OrderBy(x => x.Name, StringComparer.Ordinal))
        {
            result[property.Name] = property.Value;
        }

        return result;
    }

    private static IReadOnlyList<string> ResolveFixturePaths(string manifestPath)
    {
        var json = File.ReadAllText(manifestPath, Encoding.UTF8);
        using var document = JsonDocument.Parse(json);

        var root = document.RootElement;
        var manifestDir = Path.GetDirectoryName(manifestPath) ?? Directory.GetCurrentDirectory();

        var explicitFixturePaths = ResolveExplicitFixturePaths(root, manifestDir);
        if (explicitFixturePaths.Count > 0)
        {
            return explicitFixturePaths
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(x => x, StringComparer.OrdinalIgnoreCase)
                .ToArray();
        }

        var fixturePolicy = root.TryGetProperty("fixture_policy", out var policyElement) ? policyElement : default;
        var fixtureRoot = fixturePolicy.ValueKind != JsonValueKind.Undefined && fixturePolicy.TryGetProperty("root", out var rootElement)
            ? rootElement.GetString() ?? string.Empty
            : string.Empty;
        var fixtureGlob = fixturePolicy.ValueKind != JsonValueKind.Undefined && fixturePolicy.TryGetProperty("glob", out var globElement)
            ? globElement.GetString() ?? "*.mld"
            : "*.mld";

        var effectiveFixtureRoot = string.IsNullOrWhiteSpace(fixtureRoot)
            ? manifestDir
            : Path.GetFullPath(Path.Combine(manifestDir, fixtureRoot));

        var files = Directory.Exists(effectiveFixtureRoot)
            ? Directory.GetFiles(effectiveFixtureRoot, "*", SearchOption.TopDirectoryOnly)
                .Where(path => GlobMatches(Path.GetFileName(path), fixtureGlob))
                .ToArray()
            : Array.Empty<string>();

        return files.OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private static IReadOnlyList<string> ResolveExplicitFixturePaths(JsonElement root, string manifestDir)
    {
        if (!root.TryGetProperty("fixtures", out var fixturesElement) || fixturesElement.ValueKind != JsonValueKind.Array)
        {
            return Array.Empty<string>();
        }

        var fixturePaths = new List<string>();
        foreach (var fixture in fixturesElement.EnumerateArray())
        {
            if (fixture.ValueKind != JsonValueKind.Object
                || !fixture.TryGetProperty("mld_path", out var mldPathElement)
                || mldPathElement.ValueKind != JsonValueKind.String)
            {
                continue;
            }

            var fixturePathRaw = mldPathElement.GetString();
            if (string.IsNullOrWhiteSpace(fixturePathRaw))
            {
                continue;
            }

            var fixturePath = Path.IsPathRooted(fixturePathRaw)
                ? fixturePathRaw
                : Path.GetFullPath(Path.Combine(manifestDir, fixturePathRaw));
            if (File.Exists(fixturePath))
            {
                fixturePaths.Add(fixturePath);
            }
        }

        return fixturePaths;
    }

    private static bool GlobMatches(string fileName, string globPattern)
    {
        var escaped = Regex.Escape(globPattern)
            .Replace(@"\*", ".*", StringComparison.Ordinal)
            .Replace(@"\?", ".", StringComparison.Ordinal);
        var regexPattern = "^" + escaped + "$";
        return Regex.IsMatch(fileName, regexPattern, RegexOptions.CultureInvariant);
    }

    private static void WriteReport(string outputFile, ReferenceReport report)
    {
        var json = JsonSerializer.Serialize(report, JsonOptions);
        File.WriteAllText(outputFile, json, new UTF8Encoding(false));
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    };
}

internal sealed class ReferenceReport
{
    public string Schema { get; set; } = string.Empty;

    public Fixture Fixture { get; set; } = new();

    public Reference Reference { get; set; } = new();

    public Metrics Metrics { get; set; } = new();

    public List<Diagnostic> Diagnostics { get; set; } = [];

    public Comparison Comparison { get; set; } = new();

    public List<SliceIoPair> SliceIoPairs { get; set; } = [];
}

internal sealed class Fixture
{
    public string Id { get; set; } = string.Empty;

    public string MldPath { get; set; } = string.Empty;

    public int? ModelBlockOffset { get; set; }

    public int? MotionBlockOffset { get; set; }
}

internal sealed class Reference
{
    public string Source { get; set; } = string.Empty;

    public string Tag { get; set; } = string.Empty;

    public string Commit { get; set; } = string.Empty;

    public string RunnerBranch { get; set; } = string.Empty;
}

internal sealed class Metrics
{
    public SortedDictionary<string, JsonElement> Structural { get; set; } = new(StringComparer.Ordinal);

    public SortedDictionary<string, JsonElement> Semantic { get; set; } = new(StringComparer.Ordinal);
}

internal sealed class Diagnostic
{
    public string Code { get; set; } = string.Empty;

    public string Message { get; set; } = string.Empty;

    public string Stage { get; set; } = string.Empty;

    public string Severity { get; set; } = "info";
}

internal sealed class Comparison
{
    public bool Pass { get; set; }

    public int MismatchCount { get; set; }
}

internal sealed class SliceIoPair
{
    public int Slice { get; set; }

    public List<FunctionIoPair> Pairs { get; set; } = [];
}

internal sealed class FunctionIoPair
{
    [JsonPropertyName("function_id")]
    public string FunctionId { get; set; } = string.Empty;

    [JsonPropertyName("input_fields")]
    public SortedDictionary<string, JsonElement> InputFields { get; set; } = new(StringComparer.Ordinal);

    [JsonPropertyName("output")]
    public JsonElement Output { get; set; }
}

internal sealed class ParserInvocationResult
{
    public bool Invoked { get; init; }

    public string Status { get; init; } = string.Empty;

    public Dictionary<string, JsonElement>? Structural { get; init; }

    public Dictionary<string, JsonElement>? Semantic { get; init; }

    public SortedDictionary<string, JsonElement> Outputs { get; init; } = new(StringComparer.Ordinal);

    public int? ModelBlockOffset { get; init; }

    public int? MotionBlockOffset { get; init; }

    public List<SliceIoPair> CollatedSlicePairs { get; init; } = [];

    public static ParserInvocationResult NotInvoked()
    {
        return new ParserInvocationResult
        {
            Invoked = false,
            Status = "not_configured",
            Outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["parser_binding"] = JsonSerializer.SerializeToElement("not_configured"),
            },
            CollatedSlicePairs = [],
        };
    }

    public static ParserInvocationResult Failed(string status)
    {
        return new ParserInvocationResult
        {
            Invoked = true,
            Status = status,
            Outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["parser_binding"] = JsonSerializer.SerializeToElement(status),
            },
            CollatedSlicePairs = [],
        };
    }

    public static ParserInvocationResult NotApplicable(string status)
    {
        return new ParserInvocationResult
        {
            Invoked = false,
            Status = status,
            Outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["parser_binding"] = JsonSerializer.SerializeToElement("not_applicable"),
                ["status"] = JsonSerializer.SerializeToElement(status),
            },
            Structural = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["parser_binding"] = JsonSerializer.SerializeToElement("not_applicable"),
                ["collated_slice_pairs"] = JsonSerializer.SerializeToElement(0),
            },
            Semantic = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["capture_mode"] = JsonSerializer.SerializeToElement("not_applicable"),
            },
            CollatedSlicePairs = [],
        };
    }
}

internal sealed class OperationRoute
{
    public string Operation { get; set; } = string.Empty;

    public string FunctionId { get; set; } = string.Empty;
}

internal sealed class SliceFunctionPair
{
    public int Slice { get; set; }

    public FunctionIoPair Pair { get; set; } = new();
}

internal sealed class BatchSummary
{
    public string Schema { get; set; } = string.Empty;

    public int TotalFixtures { get; set; }

    public int PassedFixtures { get; set; }

    public int FailedFixtures { get; set; }

    public int Slice { get; set; }

    public List<BatchFixtureResult> Results { get; set; } = [];
}

internal sealed class BatchFixtureResult
{
    public string FixtureId { get; set; } = string.Empty;

    public string InputPath { get; set; } = string.Empty;

    public string OutputPath { get; set; } = string.Empty;

    public bool Pass { get; set; }

    public int ErrorCount { get; set; }
}

internal sealed class BlockManifest
{
    [JsonPropertyName("schema")]
    public string Schema { get; set; } = string.Empty;

    [JsonPropertyName("fixture_id")]
    public string FixtureId { get; set; } = string.Empty;

    [JsonPropertyName("blocks")]
    public List<BlockManifestItem> Blocks { get; set; } = [];
}

internal sealed class BlockManifestItem
{
    [JsonPropertyName("index")]
    public int Index { get; set; }

    [JsonPropertyName("kind")]
    public string Kind { get; set; } = string.Empty;

    [JsonPropertyName("offset")]
    public int Offset { get; set; }

    [JsonPropertyName("size")]
    public int Size { get; set; }

    [JsonPropertyName("includes_njtl_prefix")]
    public bool IncludesNjtlPrefix { get; set; }

    [JsonPropertyName("path")]
    public string Path { get; set; } = string.Empty;
}

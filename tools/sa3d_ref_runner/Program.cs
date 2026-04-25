using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

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
        SetPropertyIfPresent(optionsType, instance, "Address", blockOffset);
        SetPropertyIfPresent(optionsType, instance, "ImageBase", blockOffset);
        SetPropertyIfPresent(optionsType, instance, "IsAnimation", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase));
        SetPropertyIfPresent(optionsType, instance, "TreatAsAnimation", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase));
        SetPropertyIfPresent(optionsType, instance, "TryAnimationFallback", blockKind.Equals("motion", StringComparison.OrdinalIgnoreCase));

        var requestedSlicesProperty = optionsType.GetProperty("RequestedSlices", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance);
        if (requestedSlicesProperty is not null && requestedSlicesProperty.CanWrite && requestedSlicesProperty.PropertyType == typeof(int[]))
        {
            requestedSlicesProperty.SetValue(instance, new[] { requestedSlice });
        }

        return instance;
    }

    private static void SetPropertyIfPresent(Type type, object target, string propertyName, object value)
    {
        var property = type.GetProperty(propertyName, System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance);
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

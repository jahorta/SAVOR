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
                RunnerBranch = "DetailedIO",
            },
            Metrics = metrics,
            Diagnostics = diagnostics.OrderBy(d => d.Code, StringComparer.Ordinal).ToList(),
            Comparison = new Comparison
            {
                Pass = !hasError,
                MismatchCount = hasError ? diagnostics.Count(x => x.Severity.Equals("error", StringComparison.OrdinalIgnoreCase)) : 0,
            },
            SliceIoPairs = new List<SliceIoPair>
            {
                new()
                {
                    Slice = slice,
                    Inputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
                    {
                        ["input_path"] = JsonSerializer.SerializeToElement(inputPath),
                        ["input_size_bytes"] = JsonSerializer.SerializeToElement(bytes.Length),
                        ["input_sha256"] = JsonSerializer.SerializeToElement(hash),
                        ["block_manifest_path"] = JsonSerializer.SerializeToElement(options.TryGetValue("--block-manifest", out var blockManifestPathRaw) ? Path.GetFullPath(blockManifestPathRaw) : string.Empty),
                        ["block_count"] = JsonSerializer.SerializeToElement(blockManifest?.Blocks.Count ?? 0),
                    },
                    Outputs = parserOutput.Outputs,
                },
            },
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
            };

            var outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["parser_binding"] = JsonSerializer.SerializeToElement("sa3d_modeling_reflection"),
                ["sa3d_modeling_dll"] = JsonSerializer.SerializeToElement(assemblyPath),
                ["parsed_object_blocks"] = JsonSerializer.SerializeToElement(parsedObject),
                ["parsed_motion_blocks"] = JsonSerializer.SerializeToElement(parsedMotion),
                ["failed_blocks"] = JsonSerializer.SerializeToElement(failedBlocks),
                ["slice"] = JsonSerializer.SerializeToElement(slice),
                ["fixture_input"] = JsonSerializer.SerializeToElement(inputPath),
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

    public SortedDictionary<string, JsonElement> Inputs { get; set; } = new(StringComparer.Ordinal);

    public SortedDictionary<string, JsonElement> Outputs { get; set; } = new(StringComparer.Ordinal);
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
        };
    }
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

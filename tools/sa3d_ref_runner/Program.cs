using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
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
        Console.WriteLine("  SA3DRefRunner run-one --input <file.mld> --out <dir> [--output-file <path>] [--manifest <path>] [--slice <n>] [--sa3d-parser-cmd <template>]");
        Console.WriteLine("  SA3DRefRunner run-all --manifest <path> --out <dir> [--slice <n>] [--sa3d-parser-cmd <template>]");
        Console.WriteLine();
        Console.WriteLine("Template placeholders for --sa3d-parser-cmd:");
        Console.WriteLine("  {input} = absolute input fixture path");
        Console.WriteLine("  {output} = absolute temp output path to write parser JSON");
        Console.WriteLine("  {slice} = current numeric slice");
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

        var parserOutput = InvokeReferenceParser(inputPath, outDir, options, slice, diagnostics);

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
                    },
                    Outputs = parserOutput.Outputs,
                },
            },
        };
    }

    private static ParserInvocationResult InvokeReferenceParser(
        string inputPath,
        string outDir,
        Dictionary<string, string> options,
        int slice,
        List<Diagnostic> diagnostics)
    {
        if (!options.TryGetValue("--sa3d-parser-cmd", out var template) || string.IsNullOrWhiteSpace(template))
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "REF_NOT_CONFIGURED",
                Severity = "warning",
                Stage = "reference_invoke",
                Message = "No --sa3d-parser-cmd provided; bridge emitted baseline report without parser invocation.",
            });

            return ParserInvocationResult.NotInvoked();
        }

        var tempOut = Path.Combine(outDir, Path.GetFileNameWithoutExtension(inputPath) + ".sa3d.reference.raw.json");
        var command = template
            .Replace("{input}", EscapeForShell(inputPath), StringComparison.Ordinal)
            .Replace("{output}", EscapeForShell(tempOut), StringComparison.Ordinal)
            .Replace("{slice}", slice.ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal);

        var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = "bash",
                ArgumentList = { "-lc", command },
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            },
        };

        process.Start();
        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit();

        if (process.ExitCode != 0)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "REF_EXEC_FAILED",
                Severity = "error",
                Stage = "reference_invoke",
                Message = $"Reference parser command exited with code {process.ExitCode}. stderr={TrimForDiagnostic(stderr)} stdout={TrimForDiagnostic(stdout)}",
            });
            return ParserInvocationResult.Failed("exec_failed");
        }

        if (!File.Exists(tempOut))
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "REF_OUTPUT_MISSING",
                Severity = "error",
                Stage = "reference_parse",
                Message = $"Reference parser command completed but did not create expected output file: {tempOut}",
            });
            return ParserInvocationResult.Failed("output_missing");
        }

        try
        {
            var json = File.ReadAllText(tempOut, Encoding.UTF8);
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;

            var structural = ReadObjectAsDictionary(root, "structural");
            var semantic = ReadObjectAsDictionary(root, "semantic");
            var outputs = new SortedDictionary<string, JsonElement>(StringComparer.Ordinal)
            {
                ["reference_output_path"] = JsonSerializer.SerializeToElement(tempOut),
                ["reference_stdout"] = JsonSerializer.SerializeToElement(stdout),
                ["reference_stderr"] = JsonSerializer.SerializeToElement(stderr),
            };

            if (root.TryGetProperty("outputs", out var outputsElement) && outputsElement.ValueKind == JsonValueKind.Object)
            {
                foreach (var property in outputsElement.EnumerateObject().OrderBy(x => x.Name, StringComparer.Ordinal))
                {
                    outputs[property.Name] = property.Value.Clone();
                }
            }

            var invocationDiagnostics = ExtractDiagnostics(root);
            diagnostics.AddRange(invocationDiagnostics);

            return new ParserInvocationResult
            {
                Invoked = true,
                Status = "ok",
                Structural = structural,
                Semantic = semantic,
                Outputs = outputs,
                ModelBlockOffset = TryGetNullableInt(root, "model_block_offset"),
                MotionBlockOffset = TryGetNullableInt(root, "motion_block_offset"),
            };
        }
        catch (Exception ex)
        {
            diagnostics.Add(new Diagnostic
            {
                Code = "REF_JSON_INVALID",
                Severity = "error",
                Stage = "reference_parse",
                Message = $"Reference parser output JSON failed to parse: {ex.Message}",
            });
            return ParserInvocationResult.Failed("invalid_json");
        }
    }

    private static Dictionary<string, JsonElement>? ReadObjectAsDictionary(JsonElement root, string name)
    {
        if (!root.TryGetProperty(name, out var value) || value.ValueKind != JsonValueKind.Object)
        {
            return null;
        }

        return value.EnumerateObject()
            .OrderBy(x => x.Name, StringComparer.Ordinal)
            .ToDictionary(x => x.Name, x => x.Value.Clone(), StringComparer.Ordinal);
    }

    private static int? TryGetNullableInt(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out var value))
        {
            return null;
        }

        if (value.ValueKind == JsonValueKind.Null)
        {
            return null;
        }

        if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var intValue))
        {
            return intValue;
        }

        return null;
    }

    private static List<Diagnostic> ExtractDiagnostics(JsonElement root)
    {
        var diagnostics = new List<Diagnostic>();
        if (!root.TryGetProperty("diagnostics", out var diagnosticsElement) || diagnosticsElement.ValueKind != JsonValueKind.Array)
        {
            return diagnostics;
        }

        foreach (var item in diagnosticsElement.EnumerateArray())
        {
            var code = item.TryGetProperty("code", out var codeValue) && codeValue.ValueKind == JsonValueKind.String
                ? codeValue.GetString() ?? "REF_DIAGNOSTIC"
                : "REF_DIAGNOSTIC";
            var message = item.TryGetProperty("message", out var messageValue) && messageValue.ValueKind == JsonValueKind.String
                ? messageValue.GetString() ?? string.Empty
                : string.Empty;
            var stage = item.TryGetProperty("stage", out var stageValue) && stageValue.ValueKind == JsonValueKind.String
                ? stageValue.GetString() ?? "reference_parse"
                : "reference_parse";
            var severity = item.TryGetProperty("severity", out var severityValue) && severityValue.ValueKind == JsonValueKind.String
                ? severityValue.GetString() ?? "info"
                : "info";

            diagnostics.Add(new Diagnostic
            {
                Code = code,
                Message = message,
                Stage = stage,
                Severity = severity,
            });
        }

        return diagnostics;
    }

    private static string EscapeForShell(string path)
    {
        return "'" + path.Replace("'", "'\\''", StringComparison.Ordinal) + "'";
    }

    private static string TrimForDiagnostic(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }

        const int maxLen = 400;
        var trimmed = value.Trim();
        return trimmed.Length <= maxLen ? trimmed : trimmed[..maxLen];
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

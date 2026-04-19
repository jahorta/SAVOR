using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

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
        Console.WriteLine("  SA3DRefRunner run-one --input <file.mld> --out <dir> [--output-file <path>] [--manifest <path>] [--slice <n>]");
        Console.WriteLine("  SA3DRefRunner run-all --manifest <path> --out <dir> [--slice <n>]");
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
        if (!options.TryGetValue("--input", out var inputPathRaw) || string.IsNullOrWhiteSpace(inputPathRaw))
        {
            Console.Error.WriteLine("run-one requires --input");
            return 2;
        }

        var inputPath = Path.GetFullPath(inputPathRaw);
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

        var manifestPath = options.TryGetValue("--manifest", out var manifestRaw) ? manifestRaw : string.Empty;
        var slice = options.TryGetValue("--slice", out var sliceRaw) && int.TryParse(sliceRaw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsedSlice)
            ? parsedSlice
            : 0;

        var bytes = File.ReadAllBytes(inputPath);
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

        var report = new
        {
            schema = "parity_report_v1",
            generator = "sa3d_ref_runner_framework",
            status = "framework_only",
            fixture = new
            {
                id = Path.GetFileNameWithoutExtension(inputPath),
                mld_path = inputPath,
                model_block_offset = (int?)null,
                motion_block_offset = (int?)null,
            },
            reference = new
            {
                source = "SA3D.Modeling",
                runner_branch = "DetailedIO",
                commit = "framework_pending",
                manifest = manifestPath,
            },
            metrics = new
            {
                structural = new
                {
                    input_size_bytes = bytes.Length,
                },
                semantic = new
                {
                    input_sha256 = hash,
                },
            },
            diagnostics = Array.Empty<object>(),
            comparison = new
            {
                pass = false,
                mismatch_count = 0,
                note = "Framework scaffold only. Parser binding still pending.",
            },
            slice_io_pairs = new object[]
            {
                new
                {
                    slice,
                    inputs = new
                    {
                        input_path = inputPath,
                        input_size_bytes = bytes.Length,
                        input_sha256 = hash,
                    },
                    outputs = new
                    {
                        parser_binding = "pending",
                        model_summary = "pending",
                        motion_summary = "pending",
                    },
                },
            },
        };

        var json = JsonSerializer.Serialize(report, new JsonSerializerOptions
        {
            WriteIndented = true,
        });
        File.WriteAllText(outputFile, json, Encoding.UTF8);

        Console.WriteLine($"Wrote reference framework report: {outputFile}");
        return 0;
    }

    private static int RunAll(Dictionary<string, string> options)
    {
        if (!options.TryGetValue("--manifest", out var manifestRaw) || string.IsNullOrWhiteSpace(manifestRaw))
        {
            Console.Error.WriteLine("run-all requires --manifest");
            return 2;
        }

        var manifestPath = Path.GetFullPath(manifestRaw);
        if (!File.Exists(manifestPath))
        {
            Console.Error.WriteLine($"Manifest does not exist: {manifestPath}");
            return 1;
        }

        if (!options.TryGetValue("--out", out var outRaw) || string.IsNullOrWhiteSpace(outRaw))
        {
            Console.Error.WriteLine("run-all requires --out");
            return 2;
        }

        var outDir = Path.GetFullPath(outRaw);
        Directory.CreateDirectory(outDir);

        var json = File.ReadAllText(manifestPath, Encoding.UTF8);
        using var document = JsonDocument.Parse(json);

        var root = document.RootElement;
        var fixturePolicy = root.TryGetProperty("fixture_policy", out var policyElement) ? policyElement : default;
        var fixtureRoot = fixturePolicy.ValueKind != JsonValueKind.Undefined && fixturePolicy.TryGetProperty("root", out var rootElement)
            ? rootElement.GetString() ?? string.Empty
            : string.Empty;

        var repoRoot = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(manifestPath) ?? ".", ".."));
        var effectiveFixtureRoot = string.IsNullOrWhiteSpace(fixtureRoot)
            ? repoRoot
            : Path.GetFullPath(Path.Combine(repoRoot, fixtureRoot));

        var files = Directory.Exists(effectiveFixtureRoot)
            ? Directory.GetFiles(effectiveFixtureRoot, "*.mld", SearchOption.TopDirectoryOnly).OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray()
            : Array.Empty<string>();

        var successes = 0;
        foreach (var file in files)
        {
            var status = RunOne(new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["--input"] = file,
                ["--out"] = outDir,
                ["--manifest"] = manifestPath,
                ["--slice"] = options.TryGetValue("--slice", out var sliceRaw) ? sliceRaw : "0",
            });
            if (status == 0)
            {
                successes++;
            }
        }

        Console.WriteLine($"run-all complete: {successes}/{files.Length} fixture(s) emitted.");
        return 0;
    }
}

using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;

if (args.Length is < 3 or > 6)
{
    Console.Error.WriteLine(
        "Usage: LwPpocrExample <native-library-or-directory> <model.json> <image-or-pdf> " +
        "[ocr|recognize|pdf] [auto|text|ocr|hybrid] [dpi]");
    return 2;
}

string libraryPath = ResolveLibrary(args[0]);
string modelPath = Path.GetFullPath(args[1]);
string inputPath = Path.GetFullPath(args[2]);
bool pdfInput = args.Length >= 4 &&
    args[3].Equals("pdf", StringComparison.OrdinalIgnoreCase);
bool recognizeOnly = !pdfInput && args.Length == 4 &&
    args[3].Equals("recognize", StringComparison.OrdinalIgnoreCase);
if (!pdfInput && args.Length > 4)
{
    Console.Error.WriteLine("Image mode accepts at most one operation: 'ocr' or 'recognize'.");
    return 2;
}
if (!pdfInput && args.Length == 4 && !recognizeOnly &&
    !args[3].Equals("ocr", StringComparison.OrdinalIgnoreCase))
{
    Console.Error.WriteLine("Mode must be 'ocr' or 'recognize'.");
    return 2;
}
uint pdfMode = Native.PdfModeAuto;
uint pdfDpi = 200;
if (pdfInput)
{
    if (args.Length > 6)
    {
        Console.Error.WriteLine("PDF mode accepts [mode] and [dpi] only.");
        return 2;
    }
    if (args.Length >= 5)
    {
        try
        {
            pdfMode = ParsePdfMode(args[4]);
        }
        catch (ArgumentException ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 2;
        }
    }
    if (args.Length == 6 &&
        (!uint.TryParse(args[5], out pdfDpi) || pdfDpi == 0))
    {
        Console.Error.WriteLine("PDF dpi must be a positive integer.");
        return 2;
    }
}

Native.ConfigureResolver(libraryPath);
if (pdfInput)
{
    string? nativeDirectory = Path.GetDirectoryName(libraryPath);
    if (nativeDirectory is not null)
    {
        string bundledPdfium = Path.Combine(nativeDirectory,
            OperatingSystem.IsWindows()
                ? "pdfium.dll"
                : OperatingSystem.IsMacOS() ? "libpdfium.dylib" : "libpdfium.so");
        if (File.Exists(bundledPdfium))
        {
            Environment.SetEnvironmentVariable("LW_PPOCR_PDFIUM_LIBRARY", bundledPdfium);
            if (OperatingSystem.IsWindows())
            {
                Native.SetDllDirectory(nativeDirectory);
            }
        }
    }
}
Native.Config config = default;
Native.ConfigInit(ref config);
int managedConfigSize = Marshal.SizeOf<Native.Config>();
if (config.StructSize != managedConfigSize)
{
    throw new PlatformNotSupportedException(
        $"C ABI config size mismatch: native={config.StructSize}, managed={managedConfigSize}");
}
IntPtr manifestUtf8 = Marshal.StringToCoTaskMemUTF8(modelPath);
IntPtr handle = IntPtr.Zero;
try
{
    config.ModelManifestUtf8 = manifestUtf8;
    Check(Native.Create(ref config, out handle), handle, "OCR initialization");

    byte[] encodedInput = File.ReadAllBytes(inputPath);
    IntPtr jsonUtf8 = IntPtr.Zero;
    ulong jsonLength = 0;
    int status;
    string operation;
    if (pdfInput)
    {
        Native.PdfOptions options = default;
        Native.PdfOptionsInit(ref options);
        int managedPdfOptionsSize = Marshal.SizeOf<Native.PdfOptions>();
        if (options.StructSize != managedPdfOptionsSize)
        {
            throw new PlatformNotSupportedException(
                $"C ABI PDF options size mismatch: native={options.StructSize}, " +
                $"managed={managedPdfOptionsSize}");
        }
        options.Mode = pdfMode;
        options.Dpi = pdfDpi;
        status = Native.OcrPdfEncoded(handle, encodedInput,
            (ulong)encodedInput.LongLength, ref options,
            out jsonUtf8, out jsonLength);
        operation = $"PDF OCR (mode={PdfModeName(pdfMode)}, dpi={pdfDpi})";
    }
    else
    {
        status = recognizeOnly
            ? Native.RecognizeEncoded(handle, encodedInput, (ulong)encodedInput.LongLength,
                out jsonUtf8, out jsonLength)
            : Native.OcrEncoded(handle, encodedInput, (ulong)encodedInput.LongLength,
                out jsonUtf8, out jsonLength);
        operation = recognizeOnly ? "recognition" : "OCR";
    }
    Check(status, handle, operation);
    try
    {
        string json = Marshal.PtrToStringUTF8(
            jsonUtf8, checked((int)jsonLength)) ?? "{}";
        using JsonDocument document = JsonDocument.Parse(json);
        Console.WriteLine(JsonSerializer.Serialize(document.RootElement,
            new JsonSerializerOptions { WriteIndented = true }));
    }
    finally
    {
        Native.StringFree(jsonUtf8);
    }
}
finally
{
    Native.Destroy(ref handle);
    Marshal.FreeCoTaskMem(manifestUtf8);
}
return 0;

static void Check(int status, IntPtr handle, string operation)
{
    if (status != 0)
    {
        throw new InvalidOperationException(
            $"{operation} failed ({status}): {Native.LastError(handle)}");
    }
}

static uint ParsePdfMode(string value) => value.ToLowerInvariant() switch
{
    "auto" => Native.PdfModeAuto,
    "text" => Native.PdfModeText,
    "ocr" => Native.PdfModeOcr,
    "hybrid" => Native.PdfModeHybrid,
    _ => throw new ArgumentException(
        "PDF mode must be 'auto', 'text', 'ocr', or 'hybrid'.")
};

static string PdfModeName(uint value) => value switch
{
    Native.PdfModeText => "text",
    Native.PdfModeOcr => "ocr",
    Native.PdfModeHybrid => "hybrid",
    _ => "auto"
};

static string ResolveLibrary(string value)
{
    string path = Path.GetFullPath(value);
    if (!Directory.Exists(path))
    {
        return path;
    }
    string name = OperatingSystem.IsWindows()
        ? "lw.PPOCR.OpenCVDNN.dll"
        : OperatingSystem.IsMacOS()
            ? "liblw.PPOCR.OpenCVDNN.dylib"
            : "liblw.PPOCR.OpenCVDNN.so";
    return Path.Combine(path, name);
}

internal static class Native
{
    private const string LibraryName = "lw.PPOCR.OpenCVDNN";
    private static string? _libraryPath;

    internal const uint PdfModeAuto = 0;
    internal const uint PdfModeText = 1;
    internal const uint PdfModeOcr = 2;
    internal const uint PdfModeHybrid = 3;

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct Config
    {
        internal uint StructSize;
        internal uint ApiVersion;
        internal IntPtr ModelManifestUtf8;
        internal int EnableClassifier;
        internal int LimitSideLen;
        internal float DetDbThreshold;
        internal float DetDbBoxThreshold;
        internal float DetDbUnclipRatio;
        internal int DetUseDilation;
        internal float ClsThreshold;
        internal int ClsBatchSize;
        internal int RecBatchSize;
        internal int RecConcurrency;
        internal ulong MaxImagePixels;
        internal int LogLevel;
        internal IntPtr LogCallback;
        internal IntPtr LogUserData;
        internal uint MaxBatchImages;
        internal uint ReservedBatchU32;
        internal ulong MaxBatchTotalPixels;
        internal ulong MaxBatchDecodedBytes;
        internal fixed int ReservedI32[2];
        internal IntPtr ReservedPtr0;
        internal IntPtr ReservedPtr1;
        internal IntPtr ReservedPtr2;
        internal IntPtr ReservedPtr3;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PdfOptions
    {
        internal uint StructSize;
        internal uint ApiVersion;
        internal uint Mode;
        internal uint Dpi;
        internal uint FirstPage;
        internal uint PageCount;
        internal uint MaxPages;
        internal uint ReservedU32;
        internal ulong MaxPagePixels;
        internal ulong MaxTotalPixels;
        internal uint Reserved0;
        internal uint Reserved1;
        internal uint Reserved2;
        internal uint Reserved3;
    }

    internal static void ConfigureResolver(string libraryPath)
    {
        _libraryPath = libraryPath;
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, Resolve);
    }

    private static IntPtr Resolve(
        string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName == LibraryName && _libraryPath is not null)
        {
            return NativeLibrary.Load(_libraryPath);
        }
        return IntPtr.Zero;
    }

    internal static string LastError(IntPtr handle)
    {
        ulong required = GetLastError(handle, IntPtr.Zero, 0);
        if (required is 0 or > 1_048_576)
        {
            return "unknown native OCR error";
        }
        IntPtr buffer = Marshal.AllocHGlobal(checked((int)required));
        try
        {
            GetLastError(handle, buffer, required);
            return Marshal.PtrToStringUTF8(buffer) ?? "unknown native OCR error";
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_config_init",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void ConfigInit(ref Config config);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_create",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Create(ref Config config, out IntPtr handle);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_ocr_encoded",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OcrEncoded(IntPtr handle, byte[] encodedImage,
        ulong encodedSize, out IntPtr resultJsonUtf8, out ulong resultJsonLength);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_pdf_options_init",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void PdfOptionsInit(ref PdfOptions options);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_pdfium_is_available",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int PdfiumIsAvailable();

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_ocr_pdf_encoded",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int OcrPdfEncoded(IntPtr handle, byte[] pdfData,
        ulong pdfSize, ref PdfOptions options,
        out IntPtr resultJsonUtf8, out ulong resultJsonLength);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_recognize_encoded",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RecognizeEncoded(IntPtr handle, byte[] encodedImage,
        ulong encodedSize, out IntPtr resultJsonUtf8, out ulong resultJsonLength);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_string_free",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void StringFree(IntPtr value);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_get_last_error",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong GetLastError(
        IntPtr handle, IntPtr bufferUtf8, ulong bufferCapacity);

    [DllImport(LibraryName, EntryPoint = "lw_ppocr_destroy",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern void Destroy(ref IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    internal static extern bool SetDllDirectory(string path);
}

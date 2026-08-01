using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;

if (args.Length is < 3 or > 4)
{
    Console.Error.WriteLine(
        "Usage: LwPpocrExample <native-library-or-directory> <model.json> <image> [ocr|recognize]");
    return 2;
}

string libraryPath = ResolveLibrary(args[0]);
string modelPath = Path.GetFullPath(args[1]);
string imagePath = Path.GetFullPath(args[2]);
bool recognizeOnly = args.Length == 4 &&
    args[3].Equals("recognize", StringComparison.OrdinalIgnoreCase);
if (args.Length == 4 && !recognizeOnly &&
    !args[3].Equals("ocr", StringComparison.OrdinalIgnoreCase))
{
    Console.Error.WriteLine("Mode must be 'ocr' or 'recognize'.");
    return 2;
}

Native.ConfigureResolver(libraryPath);
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

    byte[] encodedImage = File.ReadAllBytes(imagePath);
    IntPtr jsonUtf8 = IntPtr.Zero;
    ulong jsonLength = 0;
    int status = recognizeOnly
        ? Native.RecognizeEncoded(handle, encodedImage, (ulong)encodedImage.LongLength,
            out jsonUtf8, out jsonLength)
        : Native.OcrEncoded(handle, encodedImage, (ulong)encodedImage.LongLength,
            out jsonUtf8, out jsonLength);
    Check(status, handle, recognizeOnly ? "recognition" : "OCR");
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
}

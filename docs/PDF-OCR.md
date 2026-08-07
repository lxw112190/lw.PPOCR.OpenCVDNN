# PDF OCR runtime notes

PDF support is a core document adapter, not an invoice or document-field
parser. The adapter works page by page:

1. Load the document through PDFium.
2. Extract visible text objects and map their PDF coordinates to the rendered
   page pixel coordinate system.
3. Reuse the existing OpenCV DNN OCR pipeline only for pages that need raster
   recognition. `auto` selects text-only, OCR, or hybrid processing per page.

PDFium is loaded dynamically so the core library does not have a compile-time
PDFium header or ABI dependency. Put the matching runtime library beside the
application (`pdfium.dll`, `libpdfium.so`, or `libpdfium.dylib`) or set
`LW_PPOCR_PDFIUM_LIBRARY` to an explicit path. Windows packaging accepts an
approved DLL explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1 `
  -PackageDir dist/lw.PPOCR.OpenCVDNN-v1.1.0-windows-x64 `
  -PdfiumDll C:/path/to/pdfium.dll
```

On Windows the explicit path is read from the Unicode environment block and
loaded with `LoadLibraryW`, so paths containing Chinese or other non-ASCII
characters are supported.

The resulting Windows package places it next to the service executable, so the
web PDF test works without an extra environment variable. PDFium calls are
serialized by a process-wide lock because the public PDFium API is not
generally thread-safe.

Linux and macOS packages accept an optional PDFium runtime at packaging time:

```bash
bash scripts/package_linux.sh build/ci .ci/opencv-5.0.0 dist 1.1.0 x64 \
  /opt/pdfium/x86_64/libpdfium.so
bash scripts/package_macos.sh build/ci .ci/opencv-5.0.0 dist 1.1.0 \
  /opt/pdfium/arm64/libpdfium.dylib
```

The library must match the package architecture and libc/runtime baseline. If
it is not bundled, set `LW_PPOCR_PDFIUM_LIBRARY` before starting the service;
the `/health` response then reports `capabilities.pdf_ocr.available` so a
client can detect the optional capability without sending a PDF request.

The HTTP service exposes `POST /api/pdf/ocr` with `Content-Type:
application/pdf`; the native entry point is `lw_ppocr_ocr_pdf_encoded`. Resource
limits (`max_pdf_pages`, `max_pdf_page_pixels`, and `max_pdf_total_pixels`)
protect both raster memory and request latency. Before redistribution, bundle
an approved PDFium build and record its license, version, platform, and hash in
the release dependency/SBOM manifests.

The bundled web test page also includes `pdfjs-dist` locally under `www/pdfjs`.
It renders each selected PDF page in the browser and places a transparent
overlay canvas above it. The overlay scales the returned four-point coordinates
from `pages[].image.width/height` to the browser viewport, using different
colors for `pdf_text`, `ocr`, and `hybrid` sources.

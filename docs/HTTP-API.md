# HTTP API

All OCR responses include an `X-Request-ID` header. The same value is present
in the JSON `request_id` field and can be used to locate the corresponding
runtime and access logs. `/health` also includes this header.

Default base URL: `http://127.0.0.1:8787`

If `api_key` is configured, add `X-API-Key: <secret>` to every OCR request.
`GET /health` and static web assets remain public for local readiness checks.

## `GET /health`

Returns service readiness, product version, backend, and whether an API Key is
required. It does not load or process an image.

## `POST /api/ocr`

Runs text detection, optional direction classification, and recognition.

Preferred binary request (no Base64 expansion):

```bash
curl http://127.0.0.1:8787/api/ocr \
  -H "Content-Type: image/jpeg" \
  --data-binary @image.jpg
```

`image/png`, other `image/*` values, and `application/octet-stream` are also
accepted. The request body must contain exactly one encoded image file.

Compatible JSON/Base64 request:

```json
{"image_base64":"data:image/jpeg;base64,..."}
```

The response contains the source image size, ordered text regions, four-point
boxes, confidence scores, stage timings, and a request ID.

Each detected region uses the same flattened coordinate fields as
`lw.PPOCR.Inference`. The four points are in source-image pixel coordinates and
ordered as top-left, top-right, bottom-right, and bottom-left:

```json
{
  "text": "发动机最大净功率为85kW公告批次：389",
  "score": 0.9263,
  "cls_label": 0,
  "cls_score": 0.9999,
  "x1": 256.19,
  "y1": 2638.31,
  "x2": 944.79,
  "y2": 2641.89,
  "x3": 944.52,
  "y3": 2696.29,
  "x4": 255.92,
  "y4": 2692.71
}
```

The redundant `box` array is not emitted.

## `POST /api/recognize`

Recognizes already-cropped text-line images without running detection.

Preferred binary request for one cropped image:

```bash
curl http://127.0.0.1:8787/api/recognize \
  -H "Content-Type: image/png" \
  --data-binary @cropped-text.png
```

Compatible JSON/Base64 request for one image:

```json
{"image_base64":"..."}
```

Ordered batch of 1–256 images:

```json
{"images_base64":["...","..."]}
```

Batch recognition remains JSON/Base64 because one HTTP body contains multiple
independent images. For single-image requests, binary upload avoids Base64's
roughly 33% size increase and reduces JSON encoding/decoding work.

## Status codes

| Status | Meaning |
| --- | --- |
| `200` | Request completed successfully |
| `400` | Invalid JSON, Base64, image, or parameter |
| `401` | Missing or invalid API Key |
| `413` | Request body exceeds `max_request_bytes` |
| `500` | Model inference or unexpected server error |

Every application-level error is JSON:

```json
{"ok":false,"request_id":"...","error":"..."}
```

The service deliberately excludes API Keys, request bodies, Base64 image data,
and recognized text from its logs.

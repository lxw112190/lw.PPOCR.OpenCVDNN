#!/usr/bin/env python3
"""Minimal ctypes example for lw.PPOCR.OpenCVDNN."""

import ctypes
import json
import os
import pathlib
import platform
import sys


class Config(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("model_manifest_utf8", ctypes.c_char_p),
        ("enable_classifier", ctypes.c_int32),
        ("limit_side_len", ctypes.c_int32),
        ("det_db_threshold", ctypes.c_float),
        ("det_db_box_threshold", ctypes.c_float),
        ("det_db_unclip_ratio", ctypes.c_float),
        ("det_use_dilation", ctypes.c_int32),
        ("cls_threshold", ctypes.c_float),
        ("cls_batch_size", ctypes.c_int32),
        ("rec_batch_size", ctypes.c_int32),
        ("rec_concurrency", ctypes.c_int32),
        ("max_image_pixels", ctypes.c_uint64),
        ("log_level", ctypes.c_int32),
        ("log_callback", ctypes.c_void_p),
        ("log_user_data", ctypes.c_void_p),
        ("max_batch_images", ctypes.c_uint32),
        ("reserved_batch_u32", ctypes.c_uint32),
        ("max_batch_total_pixels", ctypes.c_uint64),
        ("max_batch_decoded_bytes", ctypes.c_uint64),
        ("reserved_i32", ctypes.c_int32 * 2),
        ("reserved_ptr", ctypes.c_void_p * 4),
    ]


def library_name() -> str:
    if platform.system() == "Windows":
        return "lw.PPOCR.OpenCVDNN.dll"
    if platform.system() == "Darwin":
        return "liblw.PPOCR.OpenCVDNN.dylib"
    return "liblw.PPOCR.OpenCVDNN.so"


def last_error(lib, handle) -> str:
    required = lib.lw_ppocr_get_last_error(handle, None, 0)
    buffer = ctypes.create_string_buffer(max(required, 1))
    lib.lw_ppocr_get_last_error(handle, buffer, len(buffer))
    return buffer.value.decode("utf-8", errors="replace")


def main() -> int:
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <library-dir> <model.json> <image>")
        return 2
    lib_path = pathlib.Path(sys.argv[1]).resolve() / library_name()
    model_path = pathlib.Path(sys.argv[2]).resolve()
    image = pathlib.Path(sys.argv[3]).read_bytes()
    dll_directory = None
    if platform.system() == "Windows" and hasattr(os, "add_dll_directory"):
        dll_directory = os.add_dll_directory(str(lib_path.parent))
    lib = ctypes.CDLL(str(lib_path))

    handle = ctypes.c_void_p()
    config = Config()
    lib.lw_ppocr_config_init.argtypes = [ctypes.POINTER(Config)]
    lib.lw_ppocr_create.argtypes = [ctypes.POINTER(Config), ctypes.POINTER(ctypes.c_void_p)]
    lib.lw_ppocr_create.restype = ctypes.c_int32
    lib.lw_ppocr_ocr_encoded.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint64)]
    lib.lw_ppocr_ocr_encoded.restype = ctypes.c_int32
    lib.lw_ppocr_get_last_error.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64]
    lib.lw_ppocr_get_last_error.restype = ctypes.c_uint64
    lib.lw_ppocr_string_free.argtypes = [ctypes.c_void_p]
    lib.lw_ppocr_destroy.argtypes = [ctypes.POINTER(ctypes.c_void_p)]

    lib.lw_ppocr_config_init(ctypes.byref(config))
    model_bytes = str(model_path).encode("utf-8")
    config.model_manifest_utf8 = model_bytes
    status = lib.lw_ppocr_create(ctypes.byref(config), ctypes.byref(handle))
    if status != 0:
        raise RuntimeError(last_error(lib, handle))
    try:
        image_buffer = ctypes.create_string_buffer(image)
        output = ctypes.c_void_p()
        output_length = ctypes.c_uint64()
        status = lib.lw_ppocr_ocr_encoded(handle, image_buffer, len(image),
            ctypes.byref(output), ctypes.byref(output_length))
        if status != 0:
            raise RuntimeError(last_error(lib, handle))
        try:
            text = ctypes.string_at(output, output_length.value).decode("utf-8")
            print(json.dumps(json.loads(text), ensure_ascii=False, indent=2))
        finally:
            lib.lw_ppocr_string_free(output)
    finally:
        lib.lw_ppocr_destroy(ctypes.byref(handle))
        if dll_directory is not None:
            dll_directory.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

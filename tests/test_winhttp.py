"""WinHTTP ile DeepL'e ham istek denemesi. Anahtar ortam değişkeninden okunur:
    $env:DEEPL_API_KEY = '...:fx'; py tests/test_winhttp.py
"""
import ctypes
import os
import sys
from ctypes import wintypes, c_void_p, c_wchar_p, c_char_p, c_uint32, byref

API_KEY = os.environ.get('DEEPL_API_KEY', '')
if not API_KEY:
    sys.exit('DEEPL_API_KEY ortam değişkeni tanımlı değil.')
HOST = 'api-free.deepl.com' if API_KEY.endswith(':fx') else 'api.deepl.com'

winhttp = ctypes.windll.winhttp
winhttp.WinHttpOpen.restype = c_void_p
winhttp.WinHttpOpen.argtypes = [c_wchar_p, c_uint32, c_wchar_p, c_wchar_p, c_uint32]
winhttp.WinHttpConnect.restype = c_void_p
winhttp.WinHttpConnect.argtypes = [c_void_p, c_wchar_p, c_uint32, c_uint32]
winhttp.WinHttpOpenRequest.restype = c_void_p
winhttp.WinHttpOpenRequest.argtypes = [c_void_p, c_wchar_p, c_wchar_p, c_wchar_p, c_wchar_p, c_void_p, c_uint32]
winhttp.WinHttpAddRequestHeaders.restype = wintypes.BOOL
winhttp.WinHttpAddRequestHeaders.argtypes = [c_void_p, c_wchar_p, c_uint32, c_uint32]
winhttp.WinHttpSendRequest.restype = wintypes.BOOL
winhttp.WinHttpSendRequest.argtypes = [c_void_p, c_wchar_p, c_uint32, c_char_p, c_uint32, c_uint32, c_void_p]
winhttp.WinHttpReceiveResponse.restype = wintypes.BOOL
winhttp.WinHttpReceiveResponse.argtypes = [c_void_p, c_void_p]
winhttp.WinHttpReadData.restype = wintypes.BOOL
winhttp.WinHttpReadData.argtypes = [c_void_p, c_char_p, c_uint32, ctypes.POINTER(wintypes.DWORD)]

hSession = winhttp.WinHttpOpen('Makine-Live/1.0', 0, None, None, 0)
hConnect = winhttp.WinHttpConnect(hSession, HOST, 443, 0)
hReq = winhttp.WinHttpOpenRequest(hConnect, 'POST', '/v2/translate', None, None, None, 0x00800000)

headers = f'Content-Type: application/json\r\nAuthorization: DeepL-Auth-Key {API_KEY}\r\n'
winhttp.WinHttpAddRequestHeaders(hReq, headers, -1, 0x20000000)

body = b'{"text":["Hello world"],"source_lang":"EN","target_lang":"TR"}'
ok1 = winhttp.WinHttpSendRequest(hReq, None, 0, body, len(body), len(body), None)
ok2 = winhttp.WinHttpReceiveResponse(hReq, None)

buf = ctypes.create_string_buffer(4096)
bytesRead = wintypes.DWORD()
winhttp.WinHttpReadData(hReq, buf, 4096, byref(bytesRead))
print('Send:', ok1, 'Recv:', ok2, 'Bytes:', bytesRead.value)
print('Response:', buf.raw[:bytesRead.value].decode('utf-8', errors='replace'))

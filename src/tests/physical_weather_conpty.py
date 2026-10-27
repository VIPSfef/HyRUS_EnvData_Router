"""Small Windows ConPTY host; terminal frames stay in bounded RAM.
Reference: https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session
No windows are shown, no screen files, keyboard automation, or external sockets.
"""
import ctypes as c
from ctypes import wintypes as w
import subprocess
import threading
import time

CAP = 8 * 1024 * 1024


class COORD(c.Structure):
    _fields_ = [("X", c.c_short), ("Y", c.c_short)]


class STARTUPINFO(c.Structure):
    _fields_ = [("cb", w.DWORD), ("lpReserved", w.LPWSTR), ("lpDesktop", w.LPWSTR), ("lpTitle", w.LPWSTR),
                ("dwX", w.DWORD), ("dwY", w.DWORD), ("dwXSize", w.DWORD), ("dwYSize", w.DWORD),
                ("dwXCountChars", w.DWORD), ("dwYCountChars", w.DWORD), ("dwFillAttribute", w.DWORD),
                ("dwFlags", w.DWORD), ("wShowWindow", w.WORD), ("cbReserved2", w.WORD), ("lpReserved2", c.POINTER(w.BYTE)),
                ("hStdInput", w.HANDLE), ("hStdOutput", w.HANDLE), ("hStdError", w.HANDLE)]


class STARTUPINFOEX(c.Structure):
    _fields_ = [("StartupInfo", STARTUPINFO), ("lpAttributeList", w.LPVOID)]


class PROCESS_INFORMATION(c.Structure):
    _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE), ("dwProcessId", w.DWORD), ("dwThreadId", w.DWORD)]


class PseudoConsole:
    def __init__(self, argv, columns=140, rows=40):
        self.k = c.WinDLL("kernel32", use_last_error=True)
        self._api()
        self.input_read, self.input_write, self.output_read, self.output_write = (w.HANDLE() for _ in range(4))
        self.hpc = w.HANDLE()
        self.process = PROCESS_INFORMATION()
        self.closed = False
        self.overflow = False
        self.size = 0
        self.parts = []
        self.ready = threading.Event()
        self.thread = None
        self.attribute_initialized = False
        self._check(self.k.CreatePipe(c.byref(self.input_read), c.byref(self.input_write), None, 0))
        self._check(self.k.CreatePipe(c.byref(self.output_read), c.byref(self.output_write), None, 0))
        hr = self.k.CreatePseudoConsole(COORD(columns, rows), self.input_read, self.output_write, 0, c.byref(self.hpc))
        if hr < 0:
            self.close()
            raise OSError("CreatePseudoConsole failed")
        self.thread = threading.Thread(target=self._drain, daemon=True)
        self.thread.start()
        required = c.c_size_t()
        self.k.InitializeProcThreadAttributeList(None, 1, 0, c.byref(required))
        self.attribute_storage = c.create_string_buffer(required.value)
        self.si = STARTUPINFOEX()
        self.si.StartupInfo.cb = c.sizeof(self.si)
        # Prevent redirected parent stdio bypassing ConPTY (Microsoft terminal #15814).
        self.si.StartupInfo.dwFlags = 0x00000100  # STARTF_USESTDHANDLES, all three handles null
        self.si.lpAttributeList = c.cast(self.attribute_storage, w.LPVOID)
        self._check(self.k.InitializeProcThreadAttributeList(self.si.lpAttributeList, 1, 0, c.byref(required)))
        self.attribute_initialized = True
        self._check(self.k.UpdateProcThreadAttribute(self.si.lpAttributeList, 0, 0x00020016, self.hpc, c.sizeof(self.hpc), None, None))
        command = c.create_unicode_buffer(subprocess.list2cmdline([str(item) for item in argv]))
        self._check(self.k.CreateProcessW(None, command, None, None, False, 0x00080000, None, None, c.byref(self.si), c.byref(self.process)))
        self.k.CloseHandle(self.process.hThread)
        self.process.hThread = None
        for handle in (self.input_read, self.output_write):
            self.k.CloseHandle(handle)
        self.input_read = self.output_write = None

    def _api(self):
        declarations = {
            "CreatePipe": ([c.POINTER(w.HANDLE), c.POINTER(w.HANDLE), w.LPVOID, w.DWORD], w.BOOL),
            "CreatePseudoConsole": ([COORD, w.HANDLE, w.HANDLE, w.DWORD, c.POINTER(w.HANDLE)], c.c_long),
            "ClosePseudoConsole": ([w.HANDLE], None),
            "InitializeProcThreadAttributeList": ([w.LPVOID, w.DWORD, w.DWORD, c.POINTER(c.c_size_t)], w.BOOL),
            "UpdateProcThreadAttribute": ([w.LPVOID, w.DWORD, c.c_size_t, w.LPVOID, c.c_size_t, w.LPVOID, w.LPVOID], w.BOOL),
            "DeleteProcThreadAttributeList": ([w.LPVOID], None),
            "CreateProcessW": ([w.LPCWSTR, w.LPWSTR, w.LPVOID, w.LPVOID, w.BOOL, w.DWORD, w.LPVOID, w.LPCWSTR, w.LPVOID, c.POINTER(PROCESS_INFORMATION)], w.BOOL),
            "ReadFile": ([w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID], w.BOOL),
            "WriteFile": ([w.HANDLE, w.LPCVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID], w.BOOL),
            "WaitForSingleObject": ([w.HANDLE, w.DWORD], w.DWORD),
            "GetExitCodeProcess": ([w.HANDLE, c.POINTER(w.DWORD)], w.BOOL),
            "TerminateProcess": ([w.HANDLE, w.UINT], w.BOOL),
            "CloseHandle": ([w.HANDLE], w.BOOL),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self.k, name)
            function.argtypes, function.restype = arguments, result

    def _check(self, result):
        if not result:
            error = c.get_last_error()
            self.close()
            raise OSError(error, "ConPTY setup failed")

    def _drain(self):
        buffer = c.create_string_buffer(8192)
        received = w.DWORD()
        tail = b""
        while self.k.ReadFile(self.output_read, buffer, len(buffer), c.byref(received), None) and received.value:
            data = buffer.raw[:received.value]
            self.size += len(data)
            if self.size <= CAP:
                self.parts.append(data)
            else:
                self.overflow = True
            tail = (tail + data)[-512:]
            if b"LISTENING" in tail:
                self.ready.set()

    def poll(self):
        if not self.process.hProcess:
            return None
        code = w.DWORD()
        self._check(self.k.GetExitCodeProcess(self.process.hProcess, c.byref(code)))
        return None if code.value == 259 else code.value

    def wait(self, timeout):
        result = self.k.WaitForSingleObject(self.process.hProcess, int(timeout * 1000))
        if result == 258:
            raise TimeoutError("ConPTY child timed out")
        if result != 0:
            raise OSError("ConPTY wait failed")
        return self.poll()

    def text(self):
        return b"".join(self.parts).decode("utf-8", errors="replace")

    def write(self, data):
        """Send input only to this test-owned pseudoconsole's child process."""
        offset = 0
        while offset < len(data):
            chunk = c.create_string_buffer(data[offset:])
            written = w.DWORD()
            self._check(self.k.WriteFile(self.input_write, chunk, len(data) - offset, c.byref(written), None))
            if not written.value:
                raise OSError("ConPTY input write made no progress")
            offset += written.value

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.process.hProcess:
            code = w.DWORD()
            if self.k.GetExitCodeProcess(self.process.hProcess, c.byref(code)) and code.value == 259:
                self.k.TerminateProcess(self.process.hProcess, 1)
                self.k.WaitForSingleObject(self.process.hProcess, 3000)
        if self.hpc:
            self.k.ClosePseudoConsole(self.hpc)
            self.hpc = None
        for name in ("input_read", "output_write", "input_write"):
            handle = getattr(self, name, None)
            if handle:
                self.k.CloseHandle(handle)
                setattr(self, name, None)
        if self.thread:
            self.thread.join(timeout=3)
        if self.output_read:
            self.k.CloseHandle(self.output_read)
            self.output_read = None
        if self.attribute_initialized:
            self.k.DeleteProcThreadAttributeList(self.si.lpAttributeList)
        if self.process.hProcess:
            self.k.CloseHandle(self.process.hProcess)
            self.process.hProcess = None

"""Keep relay input in memory and expose a menu as a byte stream, never a regular file."""
from __future__ import annotations
from contextlib import contextmanager
import ctypes
import os
from pathlib import Path
import re
import tempfile
import threading
import time
import uuid

LOGIN_KEYS={'NetworkTurnUser','NetworkTurnPass','NetworkPlayerTurnUser','NetworkPlayerTurnPass',
            'TURN_USER','TURN_PASS','CC_TEST_TURN_USER','CC_TEST_TURN_PASS'}
_ENV_LOCK=threading.RLock()


def placeholder(key):return '{TURN_PASS}' if key.endswith(('Pass','PASS')) else '{TURN_USER}'


def public_value(value, secrets=()):
    """Sanitize a prospective harness write before bytes reach the filesystem."""
    if isinstance(value,dict):
        return {key:'redacted' if key in LOGIN_KEYS and item else public_value(item,secrets) for key,item in value.items()}
    if isinstance(value,list):return [public_value(item,secrets) for item in value]
    if isinstance(value,tuple):return tuple(public_value(item,secrets) for item in value)
    if not isinstance(value,str):return value
    for secret in secrets:
        if secret:value=value.replace(secret,'redacted')
    return re.sub(r'((?:Network(?:Player)?Turn(?:User|Pass)|CC_TEST_TURN_(?:USER|PASS)|TURN_(?:USER|PASS))\s*=)[^\s]+',r'\1redacted',value)


def public_settings(settings):
    return {key:'' if key in LOGIN_KEYS else value for key,value in settings.items()}


def require_public(settings):
    if any(settings.get(key) not in (None,'','redacted','{TURN_USER}','{TURN_PASS}') for key in LOGIN_KEYS):
        raise ValueError('relay login cannot be staged in settings or an explicit launch environment')


@contextmanager
def inherited_environment(values):
    with _ENV_LOCK:
        old={key:os.environ.get(key) for key in values}
        os.environ.update({key:str(value) for key,value in values.items()})
        try:yield
        finally:
            for key,value in old.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value


class MenuStream:
    """One engine reads ordinary menu lines from a named pipe; no login is staged to disk."""
    def __init__(self,text):
        self.data=text.encode('utf-8');self.stopping=threading.Event();self.ready=threading.Event();self.error=None
        self.folder=None;self.handle=None
        if os.name=='nt':self.path=r'\\.\pipe\acceptance-menu-'+uuid.uuid4().hex
        else:
            self.folder=Path(tempfile.mkdtemp(prefix='acceptance-menu-'));self.path=str(self.folder/'input')
            os.mkfifo(self.path,0o600)
        self.thread=threading.Thread(target=self._serve,daemon=True);self.thread.start()
        if not self.ready.wait(10) or self.error:raise RuntimeError('private menu input stream could not start')

    def _serve(self):
        try:
            if os.name=='nt':self._windows()
            else:
                self.ready.set()
                while not self.stopping.is_set():
                    try:descriptor=os.open(self.path,os.O_WRONLY|os.O_NONBLOCK);break
                    except OSError:time.sleep(.05)
                else:return
                try:
                    os.set_blocking(descriptor,True)
                    with os.fdopen(descriptor,'wb') as stream:stream.write(self.data)
                finally:self.data=b''
        except BaseException as error:
            self.error=type(error).__name__;self.ready.set()

    def _windows(self):
        from ctypes import wintypes as W
        class Overlapped(ctypes.Structure):
            _fields_=[('Internal',ctypes.c_size_t),('InternalHigh',ctypes.c_size_t),('Offset',W.DWORD),('OffsetHigh',W.DWORD),('hEvent',W.HANDLE)]
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateNamedPipeW.argtypes=[W.LPCWSTR,W.DWORD,W.DWORD,W.DWORD,W.DWORD,W.DWORD,W.DWORD,ctypes.c_void_p];kernel.CreateNamedPipeW.restype=W.HANDLE
        kernel.CreateEventW.argtypes=[ctypes.c_void_p,W.BOOL,W.BOOL,W.LPCWSTR];kernel.CreateEventW.restype=W.HANDLE
        kernel.ConnectNamedPipe.argtypes=[W.HANDLE,ctypes.POINTER(Overlapped)];kernel.ConnectNamedPipe.restype=W.BOOL
        kernel.WriteFile.argtypes=[W.HANDLE,ctypes.c_void_p,W.DWORD,ctypes.POINTER(W.DWORD),ctypes.POINTER(Overlapped)]
        kernel.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD]
        kernel.CancelIoEx.argtypes=[W.HANDLE,ctypes.c_void_p]
        kernel.CloseHandle.argtypes=[W.HANDLE]
        kernel.GetOverlappedResult.argtypes=[W.HANDLE,ctypes.POINTER(Overlapped),ctypes.POINTER(W.DWORD),W.BOOL]
        self.handle=kernel.CreateNamedPipeW(self.path,2|0x40000000,0,1,65536,65536,0,None)
        if self.handle==ctypes.c_void_p(-1).value:raise OSError('CreateNamedPipe')
        event=kernel.CreateEventW(None,True,False,None);overlapped=Overlapped(hEvent=event)
        self.ready.set()
        try:
            connected=kernel.ConnectNamedPipe(self.handle,ctypes.byref(overlapped))
            error=ctypes.get_last_error()
            if not connected and error not in (997,535):raise OSError('ConnectNamedPipe')
            if error!=535:
                while kernel.WaitForSingleObject(event,100)!=0:
                    if self.stopping.is_set():kernel.CancelIoEx(self.handle,None);return
            kernel.CloseHandle(event);event=kernel.CreateEventW(None,True,False,None);overlapped=Overlapped(hEvent=event)
            buffer=ctypes.create_string_buffer(self.data);written=W.DWORD()
            completed=kernel.WriteFile(self.handle,buffer,len(self.data),ctypes.byref(written),ctypes.byref(overlapped))
            if not completed and ctypes.get_last_error()!=997:raise OSError('WriteFile')
            while not completed and kernel.WaitForSingleObject(event,100)!=0:
                if self.stopping.is_set():kernel.CancelIoEx(self.handle,None);return
            if not kernel.GetOverlappedResult(self.handle,ctypes.byref(overlapped),ctypes.byref(written),False):raise OSError('GetOverlappedResult')
            if written.value!=len(self.data):raise OSError('private menu stream was short')
        finally:
            kernel.CloseHandle(event);kernel.CloseHandle(self.handle);self.handle=None;self.data=b''

    def close(self):
        self.stopping.set();self.thread.join(timeout=5);self.data=b''
        if self.folder:
            Path(self.path).unlink(missing_ok=True);self.folder.rmdir()
        if self.thread.is_alive():raise RuntimeError('private menu input stream did not close')

    def __enter__(self):return self
    def __exit__(self,*_):self.close()

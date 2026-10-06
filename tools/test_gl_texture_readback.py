"""Compile the real texture reader against deterministic GL state and pixel fixtures.

No engine, GL driver or window is opened. Both platform branches must return
the same complete RGB buffer and restore every incoming pack/texture binding.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FIXTURE = r'''
#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
constexpr GLenum GL_NO_ERROR=0, GL_RGB=1, GL_BGRA=2, GL_UNSIGNED_BYTE=3;
constexpr GLenum GL_TEXTURE_2D=4, GL_TEXTURE_BINDING_2D=5;
constexpr GLenum GL_PIXEL_PACK_BUFFER=6, GL_PIXEL_PACK_BUFFER_BINDING=7;
constexpr GLenum GL_PACK_ALIGNMENT=8, GL_PACK_ROW_LENGTH=9, GL_PACK_SKIP_ROWS=10;
constexpr GLenum GL_PACK_SKIP_PIXELS=11, GL_PACK_IMAGE_HEIGHT=12, GL_PACK_SKIP_IMAGES=13;
constexpr GLenum GL_TEXTURE_WIDTH=14, GL_TEXTURE_HEIGHT=15;
std::array<GLint, 16> state{};
int actualWidth=3, actualHeight=2, reads=0;
GLenum errorState=0, formatRead=0;
bool current=true, readFails=false, restoreFails=false;
const std::array<unsigned char,18> expected={0,40,255, 240,1,16, 17,128,19, 255,0,40, 3,2,1, 100,150,200};
void* SDL_GL_GetCurrentContext() { return current ? reinterpret_cast<void*>(1) : nullptr; }
GLenum getError() { GLenum result=errorState; errorState=0; return result; }
void getInteger(GLenum name, GLint* value) { *value=state[name]; }
void bindBuffer(GLenum, GLuint value) { state[GL_PIXEL_PACK_BUFFER_BINDING]=value; }
void pixelStore(GLenum name, GLint value) { state[name]=value; }
void bindTexture(GLenum, GLuint value) {
    state[GL_TEXTURE_BINDING_2D]=value;
    if (restoreFails && value==77) errorState=1282;
}
void getDimensions(GLenum, GLint, GLenum name, GLint* value) { *value=name==GL_TEXTURE_WIDTH ? actualWidth : actualHeight; }
void getImage(GLenum, GLint, GLenum format, GLenum, void* destination) {
    ++reads; formatRead=format;
    if (readFails) { errorState=1282; return; }
    auto* bytes=static_cast<unsigned char*>(destination);
    for (std::size_t i=0; i<6; ++i) {
        if (format==GL_BGRA) {
            bytes[i*4]=expected[i*3+2]; bytes[i*4+1]=expected[i*3+1];
            bytes[i*4+2]=expected[i*3]; bytes[i*4+3]=static_cast<unsigned char>(i*37);
        } else {
            std::copy_n(expected.data()+i*3,3,bytes+i*3);
        }
    }
}
auto glad_glGetError=&getError;
auto glad_glGetIntegerv=&getInteger;
auto glad_glBindBuffer=&bindBuffer;
auto glad_glPixelStorei=&pixelStore;
auto glad_glBindTexture=&bindTexture;
auto glad_glGetTexLevelParameteriv=&getDimensions;
auto glad_glGetTexImage=&getImage;
#ifdef __APPLE__
#undef __APPLE__
#endif
#ifdef TEST_APPLE_READER
#define __APPLE__ 1
#endif
namespace RTE {
// The function below is copied verbatim from the production header at run time.
PRODUCTION_READER
}
void reset() {
    for (std::size_t i=0; i<state.size(); ++i) state[i]=static_cast<int>(i*7+1);
    state[GL_TEXTURE_BINDING_2D]=77;
    actualWidth=3; actualHeight=2; reads=0; errorState=0; formatRead=0;
    current=true; readFails=false; restoreFails=false;
}
bool expect(bool condition, const char* name) {
    if (!condition) std::cerr << "FAIL " << name << '\n';
    return condition;
}
int main() {
    bool ok=true;
    std::array<unsigned char,18> pixels{};
    std::string error;
    reset(); auto before=state;
    ok &= expect(RTE::ReadTextureRGB(99,3,2,pixels,error),"complete read");
    ok &= expect(pixels==expected && reads==1 && state==before,"all channels/pixels/pack state exact");
#ifdef TEST_APPLE_READER
    ok &= expect(formatRead==GL_BGRA,"native color words");
#else
    ok &= expect(formatRead==GL_RGB,"existing RGB branch");
#endif
    reset(); before=state;
    ok &= expect(!RTE::ReadTextureRGB(99,3,2,std::span(pixels).first(17),error) && reads==0 && state==before,"short destination");
    reset(); before=state; actualWidth=4;
    ok &= expect(!RTE::ReadTextureRGB(99,3,2,pixels,error) && reads==0 && state==before,"wrong texture size");
    reset(); before=state; errorState=1282;
    ok &= expect(!RTE::ReadTextureRGB(99,3,2,pixels,error) && reads==0 && state==before,"existing GL error");
    reset(); before=state; readFails=true;
    ok &= expect(!RTE::ReadTextureRGB(99,3,2,pixels,error) && reads==1 && state==before,"failed read restores state");
    reset(); before=state; restoreFails=true;
    ok &= expect(!RTE::ReadTextureRGB(99,3,2,pixels,error) && state==before,"restore error remains a defect");
    reset(); before=state; current=false;
    ok &= expect(!RTE::ReadTextureRGB(99,3,2,pixels,error) && reads==0 && state==before,"no context refuses");
    std::cout << (ok ? "PASS 7/7" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header', type=Path, default=Path(__file__).resolve().parents[1] / 'Source/Renderer/GLFrameReadback.h')
    parser.add_argument('--cxx', default=os.environ.get('CXX'))
    options = parser.parse_args()
    compiler = options.cxx or next((name for name in ('c++', 'clang++', 'g++', 'cl') if shutil.which(name)), None)
    if not compiler:
        parser.error('a C++20 compiler is required; pass --cxx or use the native build environment')
    header = options.header.read_text(encoding='utf-8')
    start = header.index('\tinline bool ReadTextureRGB(')
    end = header.index('\n\t/// The GPU owns', start)
    source = FIXTURE.replace('PRODUCTION_READER', header[start:end].strip())
    with tempfile.TemporaryDirectory(prefix='texture-readback-') as temporary:
        root = Path(temporary)
        cpp = root / 'readback.cpp'
        cpp.write_text(source, encoding='utf-8')
        for apple in (False, True):
            binary = root / ('reader-apple' if apple else 'reader-rgb')
            if os.name == 'nt':
                binary = binary.with_suffix('.exe')
            if Path(compiler).stem.lower() == 'cl':
                argv = [compiler, '/nologo', '/std:c++20', '/EHsc', '/Fe:' + str(binary), str(cpp)]
                if apple: argv.append('/DTEST_APPLE_READER')
            else:
                argv = [compiler, '-std=c++20', '-O2', str(cpp), '-o', str(binary)]
                if apple: argv.append('-DTEST_APPLE_READER')
            subprocess.run(argv, cwd=root, check=True)
            subprocess.run([str(binary)], check=True)
    print('PASS 14/14: both production branches, no engine or GL context')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

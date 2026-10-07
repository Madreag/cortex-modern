"""Compile production queued readback against deterministic fences and immutable color copies; no engine or GL context."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FIXTURE = r'''
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>
using GLenum=unsigned int; using GLuint=unsigned int; using GLint=int;
using GLsizeiptr=std::intptr_t; using GLsync=void*;
constexpr GLenum GL_NO_ERROR=0, GL_RGBA=1, GL_BGRA=2, GL_UNSIGNED_BYTE=3;
constexpr GLenum GL_TEXTURE_2D=4, GL_TEXTURE_BINDING_2D=5;
constexpr GLenum GL_PIXEL_PACK_BUFFER=6, GL_PIXEL_PACK_BUFFER_BINDING=7;
constexpr GLenum GL_PACK_ALIGNMENT=8, GL_PACK_ROW_LENGTH=9, GL_PACK_SKIP_ROWS=10;
constexpr GLenum GL_PACK_SKIP_PIXELS=11, GL_PACK_IMAGE_HEIGHT=12, GL_PACK_SKIP_IMAGES=13;
constexpr GLenum GL_TEXTURE_WIDTH=14, GL_TEXTURE_HEIGHT=15, GL_STREAM_READ=16;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE=17, GL_ALREADY_SIGNALED=18;
constexpr GLenum GL_CONDITION_SATISFIED=19, GL_TIMEOUT_EXPIRED=20, GL_WAIT_FAILED=21;
std::array<GLint,16> state{};
std::map<GLuint,std::vector<unsigned char>> buffers;
const std::array<unsigned char,18> original={0,40,255, 240,1,16, 17,128,19, 255,0,40, 3,2,1, 100,150,200};
std::array<unsigned char,18> texture=original;
int actualWidth=3, actualHeight=2, reads=0, deletedBuffers=0, deletedFences=0, waits=0;
GLenum errorState=0, fenceState=GL_TIMEOUT_EXPIRED;
std::uint64_t lastTimeout=999;
bool current=true, copyFails=false, restoreFails=false, transferFails=false, allocationFails=false;
void* SDL_GL_GetCurrentContext() { return current ? reinterpret_cast<void*>(1) : nullptr; }
GLenum getError() { GLenum value=errorState;errorState=0;return value; }
void getInteger(GLenum name,GLint* value) { *value=state[name]; }
void bindBuffer(GLenum,GLuint value) { state[GL_PIXEL_PACK_BUFFER_BINDING]=value;if(restoreFails && value==77)errorState=1282; }
void bindTexture(GLenum,GLuint value) { state[GL_TEXTURE_BINDING_2D]=value; }
void pixelStore(GLenum name,GLint value) { state[name]=value; }
void dimensions(GLenum,GLint,GLenum name,GLint* value) { *value=name==GL_TEXTURE_WIDTH?actualWidth:actualHeight; }
void genBuffers(GLint,GLuint* value) { *value=101;buffers[*value]={}; }
void deleteBuffers(GLint,const GLuint* value) { ++deletedBuffers;buffers.erase(*value); }
void bufferData(GLenum,GLsizeiptr count,const void*,GLenum) { buffers[state[GL_PIXEL_PACK_BUFFER_BINDING]].resize(count);if(allocationFails)errorState=1285; }
void getImage(GLenum,GLint,GLenum format,GLenum,void* destination) {
 if(destination!=nullptr) { errorState=1282;return; }
 if(transferFails) { errorState=1282;return; }
 auto& target=buffers.at(state[GL_PIXEL_PACK_BUFFER_BINDING]);
 for(std::size_t i=0;i<6;++i) {
  target[i*4]=texture[i*3+(format==GL_BGRA?2:0)];target[i*4+1]=texture[i*3+1];
  target[i*4+2]=texture[i*3+(format==GL_BGRA?0:2)];target[i*4+3]=static_cast<unsigned char>(i*37);
 }
}
GLsync fence(GLenum,GLuint) { return reinterpret_cast<void*>(2); }
void deleteFence(GLsync) { ++deletedFences; }
GLenum waitFence(GLsync,GLuint,std::uint64_t timeout) { ++waits;lastTimeout=timeout;return fenceState; }
void flush() {}
void getBuffer(GLenum,GLsizeiptr,GLsizeiptr count,void* destination) {
 ++reads;if(copyFails){errorState=1282;return;}
 const auto& source=buffers.at(state[GL_PIXEL_PACK_BUFFER_BINDING]);std::copy_n(source.data(),count,static_cast<unsigned char*>(destination));
}
auto glad_glGetError=&getError;auto glad_glGetIntegerv=&getInteger;auto glad_glBindBuffer=&bindBuffer;
auto glad_glBindTexture=&bindTexture;auto glad_glPixelStorei=&pixelStore;auto glad_glGetTexLevelParameteriv=&dimensions;
auto glad_glGenBuffers=&genBuffers;auto glad_glDeleteBuffers=&deleteBuffers;auto glad_glBufferData=&bufferData;
auto glad_glGetTexImage=&getImage;auto glad_glFenceSync=&fence;auto glad_glDeleteSync=&deleteFence;
auto glad_glClientWaitSync=&waitFence;auto glad_glFlush=&flush;auto glad_glGetBufferSubData=&getBuffer;
namespace RTE {
PRODUCTION_QUEUE
class FrameReadbackContext { public:
PRODUCTION_SUBMIT
};
}
void reset() {
 for(std::size_t i=0;i<state.size();++i)state[i]=static_cast<int>(i*7+1);
 state[GL_PIXEL_PACK_BUFFER_BINDING]=77;buffers.clear();texture=original;
 actualWidth=3;actualHeight=2;reads=0;deletedBuffers=0;deletedFences=0;waits=0;lastTimeout=999;
 errorState=0;fenceState=GL_TIMEOUT_EXPIRED;current=true;copyFails=false;restoreFails=false;transferFails=false;allocationFails=false;
}
int checks=0;
bool expect(bool value,const char* name) { ++checks;if(!value)std::cerr<<"FAIL "<<name<<'\n';return value; }
int main() {
 bool ok=true;std::array<unsigned char,18> pixels{};std::string error;
 for(GLenum format:{GL_RGBA,GL_BGRA}) {
  reset();error.clear();const auto before=state;
  auto frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);
  ok&=expect(frame && state==before,"submit restores every pack, texture and buffer binding");if(!frame)return 1;
  texture.fill(255);
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Pending && reads==0 && deletedBuffers==0 && deletedFences==0 && lastTimeout==0,"pending fence neither blocks nor reads nor discards");
  fenceState=GL_ALREADY_SIGNALED;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Complete,"ready immutable transfer completes");
  std::array<unsigned char,18> flipped{};std::copy_n(original.data()+9,9,flipped.data());std::copy_n(original.data(),9,flipped.data()+9);
  ok&=expect(pixels==flipped && state==before && reads==1 && deletedBuffers==1 && deletedFences==1 && !frame->buffer && !frame->ready,"all RGB pixels exact, top down, variable alpha discarded, copy owned until completion");
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,std::span(pixels).first(17),error)==RTE::TextureReadbackResult::Failed && reads==0 && deletedBuffers==1,"short RGB destination fails without reading");
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);fenceState=GL_WAIT_FAILED;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Failed && reads==0 && deletedBuffers==1 && error.find("fence failed")!=std::string::npos,"failed fence retains a named defect");
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);fenceState=GL_CONDITION_SATISFIED;copyFails=true;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Failed && state==before && deletedBuffers==1,"failed copy restores buffer binding");
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);fenceState=GL_ALREADY_SIGNALED;restoreFails=true;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Failed && state==before,"restore error remains a defect");
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);errorState=1282;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Failed && reads==0 && deletedBuffers==1,"existing GL error is reported before copy");
  reset();error.clear();actualWidth=4;
  ok&=expect(!RTE::FrameReadbackContext::Submit(99,3,2,error,format) && state==before && buffers.empty(),"wrong texture size refuses transfer");
  reset();error.clear();allocationFails=true;
  ok&=expect(!RTE::FrameReadbackContext::Submit(99,3,2,error,format) && state==before && deletedBuffers==1,"allocation failure restores state and frees transfer");
  reset();error.clear();transferFails=true;
  ok&=expect(!RTE::FrameReadbackContext::Submit(99,3,2,error,format) && state==before && deletedBuffers==1 && deletedFences==1,"transfer failure restores state and frees fence");
  reset();error.clear();current=false;
  ok&=expect(!RTE::FrameReadbackContext::Submit(99,3,2,error,format) && buffers.empty(),"no context refuses submission");
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);current=false;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error)==RTE::TextureReadbackResult::Failed && reads==0 && waits==0,"no context refuses completion");
  current=true;RTE::DiscardTextureReadback(*frame);
  reset();error.clear();frame=RTE::FrameReadbackContext::Submit(99,3,2,error,format);fenceState=GL_ALREADY_SIGNALED;
  ok&=expect(RTE::ReadQueuedTextureRGB(*frame,pixels,error,true)==RTE::TextureReadbackResult::Complete && lastTimeout==1000000,"finish drains the original fenced transfer");
 }
 if(ok)std::cout<<"PASS "<<checks<<'/'<<checks<<": queued production readback, no engine or GL context\n";
 else std::cout<<"FAIL\n";return ok?0:1;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header', type=Path, default=Path(__file__).resolve().parents[1] / 'Source/Renderer/GLFrameReadback.h')
    parser.add_argument('--cxx', default=os.environ.get('CXX'))
    args = parser.parse_args()
    compiler = args.cxx or next((name for name in ('c++', 'clang++', 'g++', 'cl') if shutil.which(name)), None)
    if not compiler:
        parser.error('a C++20 compiler is required')
    header = args.header.read_text(encoding='utf-8')
    first = header.index('\tclass QueuedTextureReadback {')
    last = header.index('\n\t/// A private shared context', first)
    submit = header.index('\t\tstatic std::unique_ptr<QueuedTextureReadback> Submit(')
    end = header.index('\n\t\tbool Complete(', submit)
    source = FIXTURE.replace('PRODUCTION_QUEUE', header[first:last].strip()).replace('PRODUCTION_SUBMIT', header[submit:end].strip())
    with tempfile.TemporaryDirectory(prefix='queued-readback-') as temporary:
        root = Path(temporary)
        cpp = root / 'queued.cpp'
        cpp.write_text(source, encoding='utf-8')
        binary = root / ('queued.exe' if os.name == 'nt' else 'queued')
        command = ([compiler, '/nologo', '/std:c++20', '/EHsc', '/Fe:' + str(binary), str(cpp)]
                   if Path(compiler).stem.lower() == 'cl' else [compiler, '-std=c++20', '-O2', str(cpp), '-o', str(binary)])
        subprocess.run(command, cwd=root, check=True)
        subprocess.run([str(binary)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

#!/bin/sh
cd thirdparty/build_ffmpeg
../ffmpeg/configure --enable-shared --disable-static --prefix=/cygwin/projects/matroska_thumbnailer/thirdparty/build_prefix/ --disable-doc --disable-programs --disable-postproc --disable-swresample --build-suffix=-lavfthumb
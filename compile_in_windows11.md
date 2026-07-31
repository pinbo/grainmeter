Here is how I build opencv5 and grainmeter in Windows 11 with [MSYS2](https://www.msys2.org).

```sh	
# MSYS2 (Software Distribution and Building Platform for Windows) 
# https://www.msys2.org/

# tools and commands I used
pacman -S mingw-w64-ucrt-x86_64-gcc
pacman -S mingw-w64-ucrt-x86_64-make
pacman -S mingw-w64-ucrt-x86_64-cmake
pacman -S make

# download opencv-5.0.0 from its github release
# https://github.com/opencv/opencv/archive/refs/tags/5.0.0.zip
cd /c/Users/junli.zhang/Downloads/opencv-5.0.0/opencv-5.0.0/
mkdir build
cd build/
cmake ..   -DCMAKE_BUILD_TYPE=Release   -DCMAKE_INSTALL_PREFIX=$HOME/opencv5   -DBUILD_LIST=core,imgproc,imgcodecs,geometry   -DBUILD_ZLIB=OFF   -DOPENCV_ENABLE_MEMALIGN=OFF   -DBUILD_SHARED_LIBS=ON   -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF   -DBUILD_opencv_apps=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF
cd ..
cmake --build build --config Release
cmake --install build # install to $HOME/opencv5
# need to add $HOME/opencv5 to path, so the app can find them.
echo $PATH
PATH=$HOME/opencv5/x64/mingw/bin:$PATH
echo $PATH

# go to grainmeter to build it
cd "C:\Users\junli.zhang\Downloads\Github\grainmeter"
cmake -B build -DCMAKE_PREFIX_PATH=$HOME/opencv5
cmake --build build

# test it
./build/grainmeter.exe --input ./test/r1-1-1.jpg

# add all required 3rd party dlls to a folder, so I can distribute them
mkdir -p dist_app
cp build/grainmeter.exe dist_app/
ldd ./build/grainmeter.exe # check all the libraries needed by grainmeter.exe
# cp these dlls to the folder except "/c/Windows/*.dll", which are system dlls.
cp $(ldd ./build/grainmeter.exe | grep -o '/ucrt64/bin/.*\.dll') dist_app/ 2>/dev/null || true
cp $(ldd ./build/grainmeter.exe | grep -o '/home/.*\.dll') dist_app/ 2>/dev/null || true
ls dist_app/
# now you can copy them to any windows 11 computer
# you can use it with powershell
```
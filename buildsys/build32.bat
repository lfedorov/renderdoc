
pushd .
	cd %0/../../
	
	mkdir build\android_arm32
	cd    build\android_arm32
	
	::set PATH=C:/Gilat-Runner/msys2-64/mingw64/bin;%PATH%
	
	goto compile_only
	
	cmake ^
		-DBUILD_ANDROID=On ^
		-DANDROID_ABI=armeabi-v7a ^
		-DENABLE_VULKAN=On ^
		-DCMAKE_MAKE_PROGRAM=mingw32-make.exe ^
		-G "MinGW Makefiles" ../..
		
	:compile_only
	
	cmake --build .
	::make -j16

	xcopy /y .\bin\org.renderdoc.renderdoccmd.arm32.apk d:\projects\git\renderdoc\x64\Development\plugins\android\
	
popd
pause
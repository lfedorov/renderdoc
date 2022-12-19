
pushd .
	cd %0/../../
	
	mkdir build\android_arm64
	cd    build\android_arm64
	
	::set PATH=C:/Gilat-Runner/msys2-64/mingw64/bin;%PATH%
	
	goto compile_only
	
	cmake ^
		-DBUILD_ANDROID=On ^
		-DANDROID_ABI=arm64-v8a ^
		-DENABLE_VULKAN=On ^
		-G "Ninja" ../..
		
	:compile_only
		
	cmake --build .
	::make -j16


	xcopy /y .\bin\org.renderdoc.renderdoccmd.arm64.apk d:\projects\git\renderdoc\x64\Development\plugins\android\
	
popd
pause
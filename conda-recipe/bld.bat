@echo on
set AETHER_LOW_MEMORY=1
set CMAKE_BUILD_PARALLEL_LEVEL=1
%PYTHON% -m pip install . -vv --no-deps --no-build-isolation
if errorlevel 1 exit 1

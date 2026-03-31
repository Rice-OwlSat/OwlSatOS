echo Initializing submodules...
git submodule update --init --recursive
if %errorlevel% neq 0 (
    echo Problem: git submodule update failed
)
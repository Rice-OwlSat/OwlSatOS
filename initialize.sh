echo "Initializing submodules..."
if ! git submodule update --init --recursive; then
    echo "Problem: git submodule update failed"
fi
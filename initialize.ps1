Write-Output "Initializing submodules..."
$Error.Clear()
$output = & git submodule update --init --recursive
if ($Error[0]) {
    Write-Output "Problem: $($output)"
}
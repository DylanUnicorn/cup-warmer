$path = "e:\software\data\VSCodeData\espproject\project\cup-warmer\managed_components\lovyan03__LovyanGFX\src\lgfx\v1\platforms\esp32\common.cpp"
if (Test-Path $path) {
    Write-Host "Found file, applying patch..."
    $content = Get-Content $path
    $content = $content -replace 'GPIO\.func_in_sel_cfg\[peripheral_sig\]\.func_sel', 'GPIO.func_in_sel_cfg[peripheral_sig].in_sel'
    $content = $content -replace 'GPIO\.func_out_sel_cfg\[pin\]\.func_sel', 'GPIO.func_out_sel_cfg[pin].out_sel'
    $content | Set-Content $path -Encoding UTF8
    Write-Host "Patch applied successfully!"
} else {
    Write-Error "Could not find LovyanGFX common.cpp at $path"
}

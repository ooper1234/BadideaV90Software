param(
    [ValidateSet('auto','v90','v92','v22bis','v22','v21')]
    [string]$Mode = 'auto'
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$config=Join-Path $root 'config\v92isp-windows.env'
if(-not (Test-Path $config)){throw "Missing $config"}
$lines=Get-Content $config
$found=$false
$out=foreach($line in $lines){
    if($line -match '^MODE='){ $found=$true; "MODE=$Mode" } else { $line }
}
if(-not $found){$out += "MODE=$Mode"}
Set-Content -Path $config -Value $out -Encoding ASCII
Write-Host "[MODEM] config MODE=$Mode"

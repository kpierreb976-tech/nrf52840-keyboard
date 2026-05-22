# Switch the current PowerShell session to UTF-8.
# Usage from project root: . .\tools\utf8-console.ps1

$utf8 = [System.Text.UTF8Encoding]::new($false)

[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

try {
    chcp 65001 | Out-Null
} catch {
    Write-Warning ("Failed to switch console code page: " + $_.Exception.Message)
}

Write-Host "PowerShell session switched to UTF-8."

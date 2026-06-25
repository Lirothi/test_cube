# Repository Instructions

## Line Endings

- Preserve the existing line ending style of every edited file.
- For C++/Windows project files in this repository, use Windows CRLF endings.
- Before finishing edits, verify touched text files do not contain mixed endings. In PowerShell, a useful check is:

```powershell
$files = @('path\to\file.cpp', 'path\to\file.h')
foreach ($f in $files) {
  [byte[]]$b = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $f).Path)
  $loneLf = 0
  $loneCr = 0
  for ($i = 0; $i -lt $b.Length; $i++) {
    if ($b[$i] -eq 13) {
      if (($i + 1) -lt $b.Length -and $b[$i + 1] -eq 10) { $i++ } else { $loneCr++ }
    } elseif ($b[$i] -eq 10) {
      $loneLf++
    }
  }
  Write-Output "$f loneLF=$loneLf loneCR=$loneCr"
}
```

- If a touched source file has mixed endings, normalize it to CRLF before the final response.

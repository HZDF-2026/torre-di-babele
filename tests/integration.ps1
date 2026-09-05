# integration.ps1 — end-to-end test against a throwaway serve instance.
# Usage: powershell -ExecutionPolicy Bypass -File tests\integration.ps1
#
# NOTE: PS 5.1 mangles JSON bodies passed inline to curl.exe (embedded double
# quotes get stripped), so all JSON bodies go through Invoke-RestMethod.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot\..

$exe = "dist\greenroom.exe"
if (-not (Test-Path $exe)) { Write-Host "FAIL: build first (build.ps1)"; exit 1 }

$data = Join-Path $env:TEMP ("greenroom-it-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $data | Out-Null
$port = 7791
$tok = "it-token-123"
$base = "http://127.0.0.1:$port"

$proc = Start-Process -FilePath (Resolve-Path $exe) -ArgumentList @(
    "serve", "--port", "$port", "--data", $data, "--token", $tok
) -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $data "serve.err")

$script:failed = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "ok    $name" }
    else { Write-Host "FAIL  $name"; $script:failed = 1 }
}

# HTTP status code via curl (URL only, no body → no quoting issues).
function Status($path) {
    (& curl.exe -s -o NUL -w "%{http_code}" "$base$path") -join ""
}

# JSON API call; error responses come back as {_status,_error} objects.
function Api($method, $path, $body = $null) {
    $h = @{ Authorization = "Bearer $tok" }
    try {
        if ($null -ne $body) {
            return Invoke-RestMethod -Uri "$base$path" -Method $method -Headers $h `
                -Body $body -ContentType "application/json"
        }
        return Invoke-RestMethod -Uri "$base$path" -Method $method -Headers $h
    } catch {
        # PS 5.1 already consumed the response stream; ErrorDetails has the body.
        $errBody = $_.ErrorDetails.Message
        if (-not $errBody -and $_.Exception.Response) {
            $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream())
            $errBody = $sr.ReadToEnd()
        }
        $code = 0
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        $parsed = $null
        if ($errBody) { try { $parsed = $errBody | ConvertFrom-Json } catch {} }
        if ($parsed) {
            $parsed | Add-Member -NotePropertyName _status -NotePropertyValue $code -Force
            return $parsed
        }
        return [pscustomobject]@{ _status = $code; _error = "$errBody" }
    }
}

try {
    $up = $false
    for ($i = 0; $i -lt 50; $i++) {
        if ((Status "/v1/status") -ne "000") { $up = $true; break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $up) { Write-Host "FAIL: serve did not come up"; exit 1 }

    # --- P1: auth ---------------------------------------------------------
    Check "401 without token" ((Status "/v1/status") -eq "401")
    Check "200 with token" ((Api "GET" "/v1/status").name -eq "greenroom")
    Check "401 with wrong token" ((Status "/v1/status") -eq "401")

    # --- P3: web UI shell without auth -------------------------------------
    $ui = (Invoke-WebRequest -UseBasicParsing -Uri "$base/").Content
    Check "web UI served without auth" ($ui.Contains("<html") -and $ui.Length -gt 1000)
    $uiApi = (Invoke-WebRequest -UseBasicParsing -Uri "$base/index.html").Content
    Check "web UI at /index.html too" ($uiApi -eq $ui)

    # --- setup rooms + messages ---------------------------------------------
    $null = Api "POST" "/v1/rooms" '{"name":"it-room"}'
    $null = Api "POST" "/v1/rooms" '{"name":"it-room2"}'
    $null = Api "POST" "/v1/rooms/it-room/say" '{"agent":"alice","type":"fact","content":"the config parser lives in src/util.cpp line 40"}'
    $null = Api "POST" "/v1/rooms/it-room/say" '{"agent":"bob","type":"ask","content":"who owns the retry logic?"}'
    $null = Api "POST" "/v1/rooms/it-room2/say" '{"agent":"carol","type":"fact","content":"retry logic lives in util.cpp too"}'

    # --- P4a: search -------------------------------------------------------
    $hits = Api "GET" "/v1/search?q=util.cpp"
    Check "search finds both rooms" ((($hits.hits | ForEach-Object room) -contains "it-room") -and (($hits.hits | ForEach-Object room) -contains "it-room2"))
    $hits = Api "GET" "/v1/search?q=util.cpp&room=it-room"
    Check "search room filter" (($hits.hits | ForEach-Object room) -notcontains "it-room2")
    $hits = Api "GET" "/v1/search?q=UTIL.CPP"
    Check "search case-insensitive" ($hits.hits.Count -ge 2)
    $hits = Api "GET" "/v1/search?q=nosuchstring-xyz"
    Check "search no-hit empty" ($hits.hits.Count -eq 0)

    # --- P2: long-poll -----------------------------------------------------
    # message ids: 1=room created, 2=alice fact, 3=bob ask; "wake up" will be 4.
    # Start-Job (not Start-Process): PS 5.1 Start-Process does not quote
    # ArgumentList entries, which would split the auth header on its space.
    $waiter = Start-Job -ScriptBlock {
        param($b, $t)
        & curl.exe -s -H "Authorization: Bearer $t" "$b/v1/rooms/it-room/wait?since=3&timeout_ms=10000"
    } -ArgumentList $base, $tok
    Start-Sleep -Milliseconds 1500
    $t0 = Get-Date
    $null = Api "POST" "/v1/rooms/it-room/say" '{"agent":"dave","type":"say","content":"wake up"}'
    $res = (Receive-Job -Job $waiter -Wait) -join ""
    Remove-Job $waiter -Force
    $dt = ((Get-Date) - $t0).TotalMilliseconds
    Check "long-poll returns new message" ($res.Contains("wake up"))
    Check "long-poll woken by notify, not timeout" ($dt -lt 5000)
    # timeout path: no new message, empty array
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r = (& curl.exe -s -H "Authorization: Bearer $tok" "$base/v1/rooms/it-room/wait?since=99&timeout_ms=1000") -join ""
    $sw.Stop()
    Check "long-poll timeout returns empty" ($r.Contains('"messages":[]') -and $sw.ElapsedMilliseconds -ge 900)

    # --- P4b: task board ---------------------------------------------------
    $tk = Api "POST" "/v1/rooms/it-room/tasks" '{"title":"fix parser bug","detail":"see fact #1","agent":"alice"}'
    Check "task create" ($tk.status -eq "open")
    $id = $tk.id

    $r = Api "POST" "/v1/rooms/it-room/tasks/$id/claim" '{"agent":"bob"}'
    Check "task claim" ($r.status -eq "claimed" -and $r.assignee -eq "bob")
    $r = Api "POST" "/v1/rooms/it-room/tasks/$id/claim" '{"agent":"carol"}'
    Check "second claim rejected" ($r._status -ge 400 -and (($r.error -join "") -match "not open"))
    $still = Api "GET" "/v1/rooms/it-room/tasks"
    Check "task still claimed by bob" (($still.tasks | Where-Object id -eq $id).assignee -eq "bob")

    $r = Api "POST" "/v1/rooms/it-room/tasks/$id/submit" '{"agent":"bob","evidence":"src/util.cpp:40 fixed, tests pass"}'
    Check "task submit" ($r.status -eq "submitted" -and $r.evidence -eq "src/util.cpp:40 fixed, tests pass")

    # evidence gate: assignee cannot verify their own submission
    $r = Api "POST" "/v1/rooms/it-room/tasks/$id/verify" '{"agent":"bob","accept":true}'
    Check "self-verify rejected" ($r._status -ge 400 -and (($r.error -join "") -match "evidence gate"))

    $r = Api "POST" "/v1/rooms/it-room/tasks/$id/verify" '{"agent":"alice","accept":true}'
    Check "verify accept -> done" ($r.status -eq "done" -and $r.verifier -eq "alice")

    # reject path reopens
    $tk2 = Api "POST" "/v1/rooms/it-room/tasks" '{"title":"second task","agent":"alice"}'
    $null = Api "POST" "/v1/rooms/it-room/tasks/$($tk2.id)/claim" '{"agent":"bob"}'
    $null = Api "POST" "/v1/rooms/it-room/tasks/$($tk2.id)/submit" '{"agent":"bob","evidence":"weak"}'
    $r = Api "POST" "/v1/rooms/it-room/tasks/$($tk2.id)/verify" '{"agent":"alice","accept":false}'
    Check "verify reject -> reopened" ($r.status -eq "open" -and $r.assignee -eq "")

    # task list
    $list = Api "GET" "/v1/rooms/it-room/tasks"
    Check "task list shows both" ((($list.tasks | ForEach-Object title) -contains "fix parser bug") -and (($list.tasks | ForEach-Object title) -contains "second task"))

    # task transitions are recorded in the message stream
    $msgs = Api "GET" "/v1/rooms/it-room/messages?since=0&limit=0&type=task"
    Check "task transitions recorded as messages" ($msgs.messages.Count -ge 6)

    # --- hash chain still intact ------------------------------------------
    $v = Api "GET" "/v1/rooms/it-room/verify"
    Check "hash chain verifies" ($v.ok -eq $true)

    # --- MCP: tools/list has all tools --------------------------------------
    $env:GREENROOM_URL = $base
    $env:GREENROOM_TOKEN = $tok
    $init = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"it","version":"0"}}}'
    $listReq = '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
    $mcpOut = ($init + "`n" + $listReq + "`n") | & $exe mcp
    $toolsLine = ($mcpOut | Where-Object { $_ -match '"id":2' }) -join ""
    Check "MCP tools/list has greenroom_verify" ($toolsLine.Contains("greenroom_verify"))
    Check "MCP tools/list has greenroom_task" ($toolsLine.Contains("greenroom_task"))
    Check "MCP tools/list has greenroom_wait" ($toolsLine.Contains("greenroom_wait"))
    Check "MCP tools/list has greenroom_search" ($toolsLine.Contains("greenroom_search"))

    # --- MCP: task/search/wait round-trips through the proxy ----------------
    $call = '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"greenroom_task","arguments":{"action":"create","room":"it-room","title":"mcp task","agent":"eve"}}}'
    $mcpOut = ($init + "`n" + $call + "`n") | & $exe mcp
    $callLine = ($mcpOut | Where-Object { $_ -match '"id":3' }) -join ""
    Check "MCP greenroom_task create" ($callLine.Contains("mcp task") -and $callLine.Contains("open"))

    $callSearch = '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"greenroom_search","arguments":{"query":"retry logic"}}}'
    $mcpOut = ($init + "`n" + $callSearch + "`n") | & $exe mcp
    $searchLine = ($mcpOut | Where-Object { $_ -match '"id":4' }) -join ""
    Check "MCP greenroom_search" ($searchLine.Contains("retry logic"))

    $callWait = '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"greenroom_wait","arguments":{"room":"it-room","since":99,"timeout_ms":500}}}'
    $mcpOut = ($init + "`n" + $callWait + "`n") | & $exe mcp
    $waitLine = ($mcpOut | Where-Object { $_ -match '"id":5' }) -join ""
    Check "MCP greenroom_wait timeout" ($waitLine.Contains("messages"))

    # --- CLI against the token server ---------------------------------------
    $cliOut = (& $exe search "util.cpp" 2>&1) -join ""
    Check "CLI search" ($cliOut.Contains("it-room"))
    $cliOut = (& $exe task list it-room 2>&1) -join ""
    Check "CLI task list" ($cliOut.Contains("fix parser bug"))
    $cliOut = (& $exe wait it-room --since 99 --timeout-ms 500 2>&1) -join ""
    Check "CLI wait timeout" ($LASTEXITCODE -eq 0)
    $cliOut = (& $exe verify it-room 2>&1) -join ""
    Check "CLI verify" ($cliOut.Contains('"ok":true'))

    # --- persistence: restart serve, data survives --------------------------
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Milliseconds 800
    $proc = Start-Process -FilePath (Resolve-Path $exe) -ArgumentList @(
        "serve", "--port", "$port", "--data", $data, "--token", $tok
    ) -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $data "serve2.err")
    Start-Sleep -Milliseconds 1500
    $list = Api "GET" "/v1/rooms/it-room/tasks"
    Check "tasks survive restart" ((($list.tasks | ForEach-Object title) -contains "fix parser bug"))
    $v = Api "GET" "/v1/rooms/it-room/verify"
    Check "hash chain verifies after restart" ($v.ok -eq $true)

    if ($script:failed -eq 0) { Write-Host "ALL INTEGRATION TESTS PASSED"; exit 0 }
    else { Write-Host "SOME INTEGRATION TESTS FAILED"; exit 1 }
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
}

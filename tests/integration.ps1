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

    # --- P7: society layer ---------------------------------------------------
    $null = Api "POST" "/v1/rooms" '{"name":"soc-room"}'

    $s = Api "GET" "/v1/rooms/soc-room/society"
    Check "society empty without goal" ($s.goal.exists -eq $false -and $s.generation -eq 0)

    $g = Api "POST" "/v1/rooms/soc-room/goal" '{"text":"make the integration test pass","criteria":"all checks green","agent":"founder"}'
    Check "goal set" ($g.goal.status -eq "open" -and $g.goal.proposer -eq "founder")
    $s = Api "GET" "/v1/rooms/soc-room/society"
    Check "gen 1 born" ($s.generation -eq 1 -and $s.generations.Count -eq 1)
    Check "genesis task created" ((Api "GET" "/v1/rooms/soc-room/tasks").tasks.Count -eq 1)

    $r = Api "POST" "/v1/rooms/soc-room/goal" '{"text":"another","agent":"founder"}'
    Check "second goal rejected 409" ($r._status -eq 409)

    $r = Api "POST" "/v1/rooms/soc-room/roles" '{"agent":"bob","role":"janitor"}'
    Check "unknown role rejected" ($r._status -ge 400)
    $r = Api "POST" "/v1/rooms/soc-room/roles" '{"agent":"bob","role":"reviewer"}'
    Check "role take reviewer" (($r.roles | Where-Object { $_.role -eq "reviewer" -and $_.agent -eq "bob" }) -ne $null)

    # finish gen 1's genesis task → drain → gen 2; role gate applies to verification
    $tk = (Api "GET" "/v1/rooms/soc-room/tasks").tasks[0]
    $null = Api "POST" "/v1/rooms/soc-room/tasks/$($tk.id)/claim" '{"agent":"alice"}'
    $null = Api "POST" "/v1/rooms/soc-room/tasks/$($tk.id)/submit" '{"agent":"alice","evidence":"assessed"}'
    $r = Api "POST" "/v1/rooms/soc-room/tasks/$($tk.id)/verify" '{"agent":"carol","accept":true}'
    Check "plain agent verify rejected (role gate)" ($r._status -ge 400 -and (($r.error -join "") -match "reviewer or tester"))
    $r = Api "POST" "/v1/rooms/soc-room/tasks/$($tk.id)/verify" '{"agent":"bob","accept":true}'
    Check "reviewer verifies task" ($r.status -eq "done")
    $s = Api "GET" "/v1/rooms/soc-room/society"
    Check "drain rolled to gen 2" ($s.generation -eq 2 -and $s.generations[0].status -eq "retired")

    # goal achievement round-trip with evidence gate + role gate
    $null = Api "POST" "/v1/rooms/soc-room/goal/achieve" '{"agent":"alice","evidence":"all checks green"}'
    $r = Api "POST" "/v1/rooms/soc-room/goal/verify" '{"agent":"alice","accept":true}'
    Check "self-verify rejected (evidence gate)" ($r._status -ge 400 -and (($r.error -join "") -match "evidence gate"))
    $r = Api "POST" "/v1/rooms/soc-room/goal/verify" '{"agent":"carol","accept":true}'
    Check "plain agent goal-verify rejected (role gate)" ($r._status -ge 400)
    $r = Api "POST" "/v1/rooms/soc-room/goal/verify" '{"agent":"bob","accept":true}'
    Check "goal achieved by reviewer" ($r.goal.status -eq "achieved" -and $r.goal.verifier -eq "bob")
    $s = Api "GET" "/v1/rooms/soc-room/society"
    Check "society closed, gen retired" ($s.generation -eq 0 -and $s.generations[-1].status -eq "retired")

    # new society: numbering stays monotonic
    $null = Api "POST" "/v1/rooms/soc-room/goal" '{"text":"second society","agent":"founder"}'
    Check "new goal births gen 3 (monotonic)" ((Api "GET" "/v1/rooms/soc-room/society").generation -eq 3)

    $null = Api "POST" "/v1/rooms/soc-room/gen/advance" '{"agent":"founder","note":"stuck"}'
    Check "gen advance forced" ((Api "GET" "/v1/rooms/soc-room/society").generation -eq 4)

    $null = Api "POST" "/v1/rooms/soc-room/goal/abandon" '{"agent":"founder","reason":"done testing"}'
    Check "goal abandoned" ((Api "GET" "/v1/rooms/soc-room/society").goal.status -eq "abandoned")

    $msgs = Api "GET" "/v1/rooms/soc-room/messages?since=0&limit=0&type=goal"
    Check "goal messages recorded" ($msgs.messages.Count -ge 4)
    $msgs = Api "GET" "/v1/rooms/soc-room/messages?since=0&limit=0&type=gen"
    Check "gen messages recorded" ($msgs.messages.Count -ge 4)
    $v = Api "GET" "/v1/rooms/soc-room/verify"
    Check "hash chain verifies with society" ($v.ok -eq $true)

    # --- P8: oracle predicate, human sovereignty, chronicle ------------------
    # A hand-rolled TCP oracle: plain sockets need no HTTP.SYS urlacl. It answers
    # every request with {"satisfied":true} — the sole judge of achievement.
    $oraclePort = 7793
    $oracleJob = Start-Job -ScriptBlock {
        param($port)
        $body = '{"satisfied":true}'
        $resp = [Text.Encoding]::ASCII.GetBytes(
            "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n" + $body)
        $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
        $l.Start()
        try {
            for (;;) {
                $c = $l.AcceptTcpClient()
                try {
                    $buf = New-Object byte[] 4096
                    $null = $c.GetStream().Read($buf, 0, $buf.Length)
                    $c.GetStream().Write($resp, 0, $resp.Length)
                    $c.GetStream().Flush()
                } catch {}
                $c.Close()
            }
        } finally { $l.Stop() }
    } -ArgumentList $oraclePort

    try {
        $oracleUp = $false
        for ($i = 0; $i -lt 30; $i++) {
            try {
                $probe = New-Object System.Net.Sockets.TcpClient
                $probe.Connect("127.0.0.1", $oraclePort)
                $probe.Close()
                $oracleUp = $true
                break
            } catch { Start-Sleep -Milliseconds 200 }
        }
        Check "oracle stub up" $oracleUp

        $null = Api "POST" "/v1/rooms" '{"name":"soc2"}'

        # goals are public and hash-chained forever — credential scan and URL
        # scheme validation refuse unsafe declarations
        $r = Api "POST" "/v1/rooms/soc2/goal" '{"text":"balance over 1000, password=hunter2","agent":"f"}'
        Check "goal with credentials refused" ($r._status -ge 400 -and (($r.error -join "") -match "credentials"))
        $r = Api "POST" "/v1/rooms/soc2/goal" '{"text":"goal","oracle":"ftp://x/oracle","agent":"f"}'
        Check "non-http oracle refused" ($r._status -ge 400 -and (($r.error -join "") -match "http://"))

        $g = Api "POST" "/v1/rooms/soc2/goal" '{"text":"bank balance over threshold","criteria":"the oracle alone judges","oracle":"http://127.0.0.1:7793/oracle","agent":"founder"}'
        Check "goal set with oracle" ($g.goal.status -eq "open" -and $g.goal.oracle -eq "http://127.0.0.1:7793/oracle")

        # a task awaiting human sign-off blocks generation turnover
        $ht = Api "POST" "/v1/rooms/soc2/tasks" '{"title":"approve the payout","detail":"sign-off","agent":"alice","human":true}'
        Check "human task created flagged" ($ht.human -eq $true)

        $null = Api "POST" "/v1/rooms/soc2/roles" '{"agent":"bob","role":"reviewer"}'
        $genesis = (Api "GET" "/v1/rooms/soc2/tasks").tasks | Where-Object { $_.title -like "Generation*" }
        $null = Api "POST" "/v1/rooms/soc2/tasks/$($genesis.id)/claim" '{"agent":"alice"}'
        $null = Api "POST" "/v1/rooms/soc2/tasks/$($genesis.id)/submit" '{"agent":"alice","evidence":"assessed"}'
        $null = Api "POST" "/v1/rooms/soc2/tasks/$($ht.id)/claim" '{"agent":"alice"}'
        $null = Api "POST" "/v1/rooms/soc2/tasks/$($ht.id)/submit" '{"agent":"alice","evidence":"payout drafted"}'
        $r = Api "POST" "/v1/rooms/soc2/tasks/$($genesis.id)/verify" '{"agent":"bob","accept":true}'
        Check "genesis verified by reviewer" ($r.status -eq "done")
        $r = Api "POST" "/v1/rooms/soc2/tasks/$($ht.id)/verify" '{"agent":"bob","accept":true}'
        Check "reviewer cannot sign for the sovereign" ($r._status -ge 400 -and (($r.error -join "") -match "human sign-off"))
        Check "human task blocks the drain" ((Api "GET" "/v1/rooms/soc2/society").generation -eq 1)

        # chronicle: budget-bounded distillation, recorded on the active gen
        $r = Api "POST" "/v1/rooms/soc2/gen/chronicle" '{"agent":"recorder","chronicle":""}'
        Check "empty chronicle refused" ($r._status -ge 400)
        $c = Api "POST" "/v1/rooms/soc2/gen/chronicle" '{"agent":"recorder","chronicle":"gen 1: oracle wired, sovereign gate held; balance verdict still open."}'
        Check "chronicle recorded" ($c.generations[0].chronicle -like "*oracle wired*" -and $c.generations[0].chronicler -eq "recorder")

        # the sovereign signs; only now does the generation turn over
        $r = Api "POST" "/v1/rooms/soc2/tasks/$($ht.id)/verify" '{"agent":"human","accept":true}'
        Check "sovereign signs the task" ($r.status -eq "done" -and $r.verifier -eq "human")
        $s = Api "GET" "/v1/rooms/soc2/society"
        Check "drain after sign-off" ($s.generation -eq 2 -and $s.generations[0].note -eq "task board drained")
        $g2 = (Api "GET" "/v1/rooms/soc2/tasks").tasks | Where-Object { $_.gen -eq 2 }
        Check "gen-2 genesis reads chronicles" ($g2.detail -match "chronicles" -and $g2.detail -match "do NOT re-read")

        # achievement is judged solely by the oracle's verdict
        $null = Api "POST" "/v1/rooms/soc2/goal/achieve" '{"agent":"alice","evidence":"balance statement"}'
        $r = Api "POST" "/v1/rooms/soc2/goal/verify" '{"agent":"bob","accept":true}'
        Check "oracle verdict gates achievement" ($r.goal.status -eq "achieved" -and $r.goal.oracleRead -match '"satisfied":true')

        # a dead oracle refuses to close the goal; gen 3 stays active for the
        # MCP/CLI chronicle round-trips below
        $null = Api "POST" "/v1/rooms/soc2/goal" '{"text":"second goal","oracle":"http://127.0.0.1:9/oracle","agent":"founder"}'
        Check "second goal births gen 3" ((Api "GET" "/v1/rooms/soc2/society").generation -eq 3)
        $null = Api "POST" "/v1/rooms/soc2/goal/achieve" '{"agent":"alice","evidence":"claims"}'
        $r = Api "POST" "/v1/rooms/soc2/goal/verify" '{"agent":"bob","accept":true}'
        Check "dead oracle refuses achievement" ($r._status -ge 400 -and (($r.error -join "") -match "oracle"))
        Check "goal stays proposed" ((Api "GET" "/v1/rooms/soc2/society").goal.status -eq "proposed")
    } finally {
        Stop-Job $oracleJob -ErrorAction SilentlyContinue
        Remove-Job $oracleJob -Force -ErrorAction SilentlyContinue
    }

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
    Check "MCP tools/list has greenroom_society" ($toolsLine.Contains("greenroom_society"))

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

    $callSoc = '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"greenroom_society","arguments":{"action":"status","room":"soc-room"}}}'
    $mcpOut = ($init + "`n" + $callSoc + "`n") | & $exe mcp
    $socLine = ($mcpOut | Where-Object { $_ -match '"id":6' }) -join ""
    Check "MCP greenroom_society status" ($socLine.Contains("soc-room") -and $socLine.Contains("abandoned") -and $socLine.Contains("reviewer"))

    $callCh = '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"greenroom_society","arguments":{"action":"gen-chronicle","room":"soc2","agent":"mcp-rec","chronicle":"mcp chronicle entry"}}}'
    $mcpOut = ($init + "`n" + $callCh + "`n") | & $exe mcp
    $chLine = ($mcpOut | Where-Object { $_ -match '"id":7' }) -join ""
    Check "MCP greenroom_society gen-chronicle" ($chLine.Contains("mcp chronicle entry") -and $chLine.Contains("mcp-rec"))

    # --- CLI against the token server ---------------------------------------
    $cliOut = (& $exe search "util.cpp" 2>&1) -join ""
    Check "CLI search" ($cliOut.Contains("it-room"))
    $cliOut = (& $exe task list it-room 2>&1) -join ""
    Check "CLI task list" ($cliOut.Contains("fix parser bug"))
    $cliOut = (& $exe wait it-room --since 99 --timeout-ms 500 2>&1) -join ""
    Check "CLI wait timeout" ($LASTEXITCODE -eq 0)
    $cliOut = (& $exe verify it-room 2>&1) -join ""
    Check "CLI verify" ($cliOut.Contains('"ok":true'))
    $cliOut = (& $exe goal show soc-room 2>&1) -join ""
    Check "CLI goal show" ($cliOut.Contains("abandoned") -and $cliOut.Contains("second society"))
    $cliOut = (& $exe gen soc-room 2>&1) -join ""
    Check "CLI gen" ($cliOut.Contains("gen 4") -and $cliOut.Contains("retired"))
    $cliOut = (& $exe role list soc-room 2>&1) -join ""
    Check "CLI role list" ($cliOut.Contains("reviewer"))
    $cliOut = (& $exe gen chronicle soc2 "cli chronicle of gen 3" 2>&1) -join ""
    Check "CLI gen chronicle" ($cliOut.Contains("cli chronicle of gen 3"))
    $cliOut = (& $exe gen soc2 2>&1) -join ""
    Check "CLI gen shows chronicle" ($cliOut.Contains("cli chronicle"))
    $cliOut = (& $exe task list soc2 2>&1) -join ""
    Check "CLI task list marks human gate" ($cliOut.Contains("[human sign-off]"))

    # --- persistence: restart serve, data survives --------------------------
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Milliseconds 800
    $proc = Start-Process -FilePath (Resolve-Path $exe) -ArgumentList @(
        "serve", "--port", "$port", "--data", $data, "--token", $tok
    ) -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $data "serve2.err")
    Start-Sleep -Milliseconds 1500
    $list = Api "GET" "/v1/rooms/it-room/tasks"
    Check "tasks survive restart" ((($list.tasks | ForEach-Object title) -contains "fix parser bug"))
    $s = Api "GET" "/v1/rooms/soc-room/society"
    Check "society survives restart" ($s.goal.status -eq "abandoned" -and $s.generations.Count -eq 4)
    $s2 = Api "GET" "/v1/rooms/soc2/society"
    Check "chronicle survives restart" ($s2.generations[2].chronicle -like "*cli chronicle*" -and $s2.generations[2].chronicler -eq "anon")
    Check "goal oracle survives restart" ($s2.goal.oracle -eq "http://127.0.0.1:9/oracle" -and $s2.goal.status -eq "proposed")
    $v = Api "GET" "/v1/rooms/it-room/verify"
    Check "hash chain verifies after restart" ($v.ok -eq $true)

    if ($script:failed -eq 0) { Write-Host "ALL INTEGRATION TESTS PASSED"; exit 0 }
    else { Write-Host "SOME INTEGRATION TESTS FAILED"; exit 1 }
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
}

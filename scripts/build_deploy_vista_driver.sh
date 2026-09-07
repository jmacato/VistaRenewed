#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
workspace=${script_dir:h}
windows_vm='Tiny11'
package_name_x64='vista-driver-x64-pnp-current'
package_name_x86='vista-driver-x86-pnp-current'
transfer_root="$workspace/aaaaa/vista-signing-transfer"
install_marker="$transfer_root/.vista-install-complete"
probe_result="$transfer_root/.triton9-probe-result"
service_result="$transfer_root/.triton9-service-result"
umd_root="$workspace/triton-umd"
umd_build_x64="$umd_root/build-vista-x64-unified"
umd_build_x86="$umd_root/build-vista-x86-unified"
umd_stage="$workspace/test-artifacts/vista-unified-umd"
source_manifest="$workspace/test-artifacts/vista-unified-source.sha256"
vista_python_bin=''
vista_mako_path=''
qmp_socket='/tmp/vista-neptune-qmp.sock'
hmp_socket='/tmp/vista-neptune-kd-pipe.sock'
debug_pointer='/tmp/vista-current-debug-log'
qemu_log_pointer='/tmp/vista-current-qemu-log'
artifact_port='8089'
qemu_binary="$workspace/triton-qemu/build/qemu-system-x86_64"
npt_backend_library="$workspace/host-triton/lib/libdxmt-native.dylib"
npt_render_server="$workspace/host-triton/libexec/virgl_render_server"
vista_disk="$workspace/winvista-3.shrunk.qcow2"
vista_deploy_iso='/tmp/vista-driver-deploy.iso'
qemu_screen='vista_aero'
kd_pacer_screen='vista_kd_pacer'
kd_pacer_log='/tmp/vista-kd-pacer-current.log'
screen_png="$workspace/.vista-deploy-screen.png"
vista_recovery_menu_visits=0
vista_last_screen_state=''
vista_allowed_driver_entries=1
vista_resume_recovery=0
vista_allowed_kd_breaks=0
vista_allowed_kd_display_errors=0
vista_allowed_kd_prompts=0
vista_allowed_taskeng_assertions=0
kd_pacer_ready=0
vista_checked_kd_active=0
vista_cleanup_armed=0
vista_cleanup_running=0

# Inf2Cat rejects a DriverVer calendar date that is ahead of UTC, even when
# both build machines are already on the next local day.  Generate both parts
# in UTC so builds around midnight remain signable and monotonically ordered.
driver_date=$(date -u '+%m/%d/%Y')
utc_hour=$(date -u '+%H')
utc_minute=$(date -u '+%M')
utc_second=$(date -u '+%S')
# Vista identifies a DirectX 9 WDDM driver with a 7.14.x.y version.  The INF
# date orders builds across days.  This revision orders builds within one UTC
# day at nine-second resolution and remains in the documented 0000-9999 range.
driver_revision=$(((10#$utc_hour * 3600 + 10#$utc_minute * 60 + 10#$utc_second) / 9))
driver_version="7.14.1.$driver_revision"
mode=${1:-'--build-only'}
runtime_only=0
probe_only=0
repackage_only=0
audit_only=0

case "$mode" in
    --build-only)
        build_package=1
        deploy_package=0
        ;;
    --restage-only)
        # UMD/service-only changes do not need a second WDK build.  Rebuild
        # the guest binaries locally, regenerate the source manifest, and
        # make fresh Vista catalogs around the already signed, source-matched
        # KMDs.  This keeps an iterative D3D9 bring-up package path short.
        build_package=1
        deploy_package=0
        repackage_only=1
        ;;
    --audit-only)
        build_package=0
        deploy_package=0
        audit_only=1
        ;;
    --all|--deploy-only|--runtime-only|--probe-only)
        printf '%s\n' \
            'host-driven Vista deployment is disabled; build the package, stage read-only media, and let TritonVistaDeploy run inside Vista' >&2
        exit 2
        ;;
    *)
        printf 'usage: %s [--audit-only|--build-only|--restage-only]\n' "$0" >&2
        exit 2
        ;;
esac

log() {
    printf '[vista-driver] %s\n' "$*"
}

# The guest appends the complete UMD DDI trace to the one-shot probe result.
# Keep the terminal transcript visible, but do not dump an unbounded log into
# the controlling PTY: a write failure there must not bypass cleanup and leave
# the exact Vista VM running.
print_d3d9_failure_summary() {
    local report=$1
    local summary=${report%%--- TRITON9-DDI-LOG*}

    if (( ${#summary} > 4096 )); then
        summary="${summary[1,4096]}"$'\n[terminal probe transcript truncated; full DDI log is on the Vista disk]'
    fi
    printf '%s\n' "$summary"
}

fail() {
    printf '[vista-driver] ERROR: %s\n' "$*" >&2
    if (( vista_cleanup_armed && ! vista_cleanup_running )); then
        vista_cleanup_running=1
        terminate_exact_vista_qemu || true
    fi
    exit 1
}

handle_interrupted_deployment() {
    trap - INT TERM HUP
    if (( vista_cleanup_armed )); then
        log "deployment interrupted; stopping the exact Vista test VM"
        vista_cleanup_running=1
        terminate_exact_vista_qemu || true
    fi
    exit 130
}

trap handle_interrupted_deployment INT TERM HUP

handle_script_exit() {
    local exit_code=$?
    if (( exit_code != 0 && vista_cleanup_armed && ! vista_cleanup_running )); then
        vista_cleanup_running=1
        log "deployment exited unexpectedly; stopping the exact Vista test VM"
        terminate_exact_vista_qemu || true
    fi
    return "$exit_code"
}

trap handle_script_exit EXIT

terminate_exact_vista_qemu() {
    local pid
    pid=$(vista_qemu_pid)
    test -n "$pid" || return 0
    if [[ "$pid" == *$'\n'* ]]; then
        fail "more than one QEMU process is using $vista_disk"
    fi
    kill -TERM "$pid" 2>/dev/null || true
    for attempt in {1..10}; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.2
    done
    # The PID was resolved from both the exact QEMU binary and the exact Vista
    # disk above.  Do not leave a crashed guest running if graceful termination
    # stalls while the script is already aborting the deployment.
    kill -KILL "$pid" 2>/dev/null || true
    for attempt in {1..10}; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    fail "could not halt exact Vista QEMU process $pid"
}

terminate_exact_artifact_server() {
    local -a pids
    local pid command_line
    pids=(${(f)"$(lsof -nP -t -iTCP:"$artifact_port" -sTCP:LISTEN 2>/dev/null || true)"})
    (( ${#pids} )) || return 0

    # A detached screen server can leave its Python child behind after the
    # controlling terminal disappears.  Resolve every listener by its full
    # argv before sending a signal; never evict an unrelated service merely
    # because it happens to use the configured port.
    for pid in $pids; do
        command_line=$(ps -p "$pid" -o command= 2>/dev/null || true)
        if [[ "$command_line" != *"$workspace/scripts/vista_artifact_server.py"* ]] || \
           [[ "$command_line" != *"--port $artifact_port"* ]] || \
           [[ "$command_line" != *"--root $transfer_root"* ]]; then
            fail "TCP $artifact_port is occupied by an unrelated process: $command_line"
        fi
    done

    kill -TERM $pids 2>/dev/null || true
    for attempt in {1..30}; do
        if ! lsof -nP -iTCP:"$artifact_port" -sTCP:LISTEN >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done

    # The targets were resolved above from the exact script, port, and root.
    kill -KILL $pids 2>/dev/null || true
    for attempt in {1..10}; do
        if ! lsof -nP -iTCP:"$artifact_port" -sTCP:LISTEN >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    fail "the exact Vista artifact server would not release TCP $artifact_port"
}

abort_vista_boot() {
    local phase=$1
    local reason=$2
    local stamp failure_root debug_log qemu_log
    stamp=$(date '+%Y%m%d-%H%M%S')
    failure_root="/tmp/vista-driver-failure-$stamp"

    if test -S "$qmp_socket"; then
        capture_vista_screen || true
    fi
    test -f "$screen_png" && cp -f "$screen_png" "$failure_root.png"
    if test -f "$screen_png"; then
        tesseract "$screen_png" "$failure_root" --psm 6 -l eng \
            >/dev/null 2>&1 || true
    fi
    if test -f "$debug_pointer"; then
        debug_log=$(<"$debug_pointer")
        test -f "$debug_log" && cp -f "$debug_log" "$failure_root-debugcon.log"
    fi
    if test -f "$qemu_log_pointer"; then
        qemu_log=$(<"$qemu_log_pointer")
        test -f "$qemu_log" && cp -f "$qemu_log" "$failure_root-qemu.log"
    fi
    prlctl exec "$windows_vm" cmd.exe /d /c \
        'type C:\VistaSigning\vista-kd-capture.log' \
        > "$failure_root-kd.log" 2>/dev/null || true

    terminate_exact_vista_qemu
    fail "$phase: $reason; VM halted, evidence prefix $failure_root"
}

vista_screen_is_blue_crash() {
    test -f "$screen_png" || return 1
    local channels
    channels=$(magick "$screen_png" -colorspace RGB \
        -format '%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]' info: 2>/dev/null) || \
        return 1
    awk '{ exit !(($3 > 0.30) && ($3 > $1 * 2.30) && ($3 > $2 * 1.70)) }' \
        <<< "$channels"
}

guard_checked_kd_bugcheck() {
    local phase=$1
    local break_count display_error_count prompt_count assertion_count
    local diagnostic_capture
    (( vista_checked_kd_active )) || return 0
    if prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"BugCheck " /c:"Fatal System Error" C:\VistaSigning\vista-kd-capture.log' \
        >/dev/null 2>&1; then
        abort_vista_boot "$phase" \
            'checked KD reported a kernel bugcheck or fatal system error'
    fi

    assertion_count=$(prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"Assertion failure - code" C:\VistaSigning\vista-kd-capture.log | find /c /v ""' \
        2>/dev/null | tr -cd '0-9\n' | tail -1 || true)
    assertion_count=${assertion_count:-0}
    if (( assertion_count > vista_allowed_taskeng_assertions )); then
        # Checked user-mode assertions stop at ntdll!int 2c before a blue
        # screen exists. Capture the process/thread and stack before deciding
        # whether this is the known unrelated taskeng.exe heap-list failure.
        send_checked_kd_command '.echo VISTA_FATAL_DIAGNOSTIC_BEGIN' || true
        send_checked_kd_command '!process -1 0' || true
        send_checked_kd_command '!thread' || true
        send_checked_kd_command 'kv' || true
        send_checked_kd_command 'u @rip-20 @rip+20' || true
        send_checked_kd_command '.echo VISTA_FATAL_DIAGNOSTIC_DONE' || true
        for capture_attempt in {1..30}; do
            if prlctl exec "$windows_vm" cmd.exe /d /c \
                'findstr /c:"VISTA_FATAL_DIAGNOSTIC_DONE" C:\VistaSigning\vista-kd-capture.log' \
                >/dev/null 2>&1; then
                break
            fi
            sleep 1
        done
        diagnostic_capture=$(prlctl exec "$windows_vm" cmd.exe /d /c \
            'type C:\VistaSigning\vista-kd-capture.log' 2>/dev/null | \
            awk '/VISTA_FATAL_DIAGNOSTIC_BEGIN/{block=""} {block=block $0 ORS} END{printf "%s", block}')
        if (( assertion_count <= 2 )) && \
           printf '%s\n' "$diagnostic_capture" | rg -qi 'Image:[[:space:]]+taskeng[.]exe'; then
            vista_allowed_taskeng_assertions=$assertion_count
            send_checked_kd_command gn || abort_vista_boot "$phase" \
                'could not continue the verified taskeng.exe assertion'
            log "continued unrelated taskeng.exe assertion $assertion_count so display validation can proceed"
            sleep 1
        else
            abort_vista_boot "$phase" \
                'checked KD reported an assertion outside the verified taskeng.exe exception'
        fi
    fi
    break_count=$(prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"Break instruction exception" C:\VistaSigning\vista-kd-capture.log | find /c /v ""' \
        2>/dev/null | tr -cd '0-9\n' | tail -1 || true)
    break_count=${break_count:-0}
    if (( break_count > vista_allowed_kd_breaks )); then
        abort_vista_boot "$phase" \
            "checked KD stopped on diagnostic breakpoint $break_count (allowed $vista_allowed_kd_breaks)"
    fi
    display_error_count=$(prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"Display Driver Model noncritical error detected" C:\VistaSigning\vista-kd-capture.log | find /c /v ""' \
        2>/dev/null | tr -cd '0-9\n' | tail -1 || true)
    display_error_count=${display_error_count:-0}
    if (( display_error_count > vista_allowed_kd_display_errors )); then
        # A checked-DXG prompt describes the symptom but not the violated DDI
        # contract.  Enter the offered break and collect the live kernel stack
        # before halting the exact test VM.  This makes the first occurrence
        # actionable and avoids another boot whose only purpose is to recover
        # the location of the same diagnostic.
        local diagnostic_break_count=$break_count diagnostic_ready=0
        send_checked_kd_command b || abort_vista_boot "$phase" \
            'could not select break at the checked display-model prompt'
        for capture_attempt in {1..30}; do
            break_count=$(prlctl exec "$windows_vm" cmd.exe /d /c \
                'findstr /i /c:"Break instruction exception" C:\VistaSigning\vista-kd-capture.log | find /c /v ""' \
                2>/dev/null | tr -cd '0-9\n' | tail -1 || true)
            break_count=${break_count:-0}
            if (( break_count > diagnostic_break_count )); then
                diagnostic_ready=1
                break
            fi
            sleep 1
        done
        if (( diagnostic_ready )); then
            send_checked_kd_command '.echo VISTA_DISPLAY_DIAGNOSTIC_BEGIN' || true
            send_checked_kd_command '!thread' || true
            send_checked_kd_command 'kv' || true
            send_checked_kd_command 'u @rip-20 @rip+20' || true
            send_checked_kd_command '.echo VISTA_DISPLAY_DIAGNOSTIC_DONE' || true
            for capture_attempt in {1..30}; do
                if prlctl exec "$windows_vm" cmd.exe /d /c \
                    'findstr /c:"VISTA_DISPLAY_DIAGNOSTIC_DONE" C:\VistaSigning\vista-kd-capture.log' \
                    >/dev/null 2>&1; then
                    break
                fi
                sleep 1
            done
        fi
        abort_vista_boot "$phase" \
            "checked KD reported display-model diagnostic $display_error_count (allowed $vista_allowed_kd_display_errors)"
    fi
    prompt_count=$(prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"Break to debug, Ignore (bi)?" C:\VistaSigning\vista-kd-capture.log | find /c /v ""' \
        2>/dev/null | tr -cd '0-9\n' | tail -1 || true)
    prompt_count=${prompt_count:-0}
    if (( prompt_count > vista_allowed_kd_prompts )); then
        # Some checked-DXG call sites emit only the prompt, without the
        # preceding "Display Driver Model noncritical error" line.  Capture
        # those with the same live-stack procedure instead of discarding the
        # only useful state when the VM is halted.
        local prompt_break_count=$break_count prompt_diagnostic_ready=0
        send_checked_kd_command b || abort_vista_boot "$phase" \
            'could not select break at the unclassified diagnostic prompt'
        for capture_attempt in {1..30}; do
            break_count=$(prlctl exec "$windows_vm" cmd.exe /d /c \
                'findstr /i /c:"Break instruction exception" C:\VistaSigning\vista-kd-capture.log | find /c /v ""' \
                2>/dev/null | tr -cd '0-9\n' | tail -1 || true)
            break_count=${break_count:-0}
            if (( break_count > prompt_break_count )); then
                prompt_diagnostic_ready=1
                break
            fi
            sleep 1
        done
        if (( prompt_diagnostic_ready )); then
            send_checked_kd_command '.echo VISTA_DISPLAY_DIAGNOSTIC_BEGIN' || true
            send_checked_kd_command '!thread' || true
            send_checked_kd_command 'kv' || true
            send_checked_kd_command 'u @rip-20 @rip+20' || true
            send_checked_kd_command '.echo VISTA_DISPLAY_DIAGNOSTIC_DONE' || true
            for capture_attempt in {1..30}; do
                if prlctl exec "$windows_vm" cmd.exe /d /c \
                    'findstr /c:"VISTA_DISPLAY_DIAGNOSTIC_DONE" C:\VistaSigning\vista-kd-capture.log' \
                    >/dev/null 2>&1; then
                    break
                fi
                sleep 1
            done
        fi
        abort_vista_boot "$phase" \
            "checked KD is waiting at an unclassified diagnostic prompt $prompt_count (allowed $vista_allowed_kd_prompts)"
    fi
}

guard_vista_runtime() {
    local phase=$1
    local text=$2
    local pid debug_log driver_entries=0 screen_state='other'

    pid=$(vista_qemu_pid)
    if test -z "$pid"; then
        abort_vista_boot "$phase" \
            'QEMU exited unexpectedly (guest reset/crash is fatal under -no-reboot)'
    fi

    if [[ "$text" == *'A problem has been detected'* ]] || \
       [[ "$text" == *'Technical information:'* ]] || \
       [[ "$text" == *'Beginning dump of physical memory'* ]] || \
       [[ "$text" == *'collecting data for crash dump'* ]] || \
       [[ "$text" == *'STOP: 0x'* ]] || \
       vista_screen_is_blue_crash; then
        abort_vista_boot "$phase" 'Vista blue screen detected'
    fi

    # DriverEntry is called once per kernel/module load.  More loads than the
    # current phase explicitly permits prove an unexpected PnP reload loop.
    # A guest reset is independently fatal because QEMU uses -no-reboot.
    if test -f "$debug_pointer"; then
        debug_log=$(<"$debug_pointer")
        if test -f "$debug_log"; then
            driver_entries=$(rg -a -c 'VIOGPU FULL build' "$debug_log" 2>/dev/null || true)
            driver_entries=${driver_entries:-0}
            if (( driver_entries > vista_allowed_driver_entries )); then
                abort_vista_boot "$phase" \
                    "boot loop detected: DriverEntry appeared $driver_entries times (allowed $vista_allowed_driver_entries)"
            fi
        fi
    fi

    if [[ "$text" == *'Windows Error Recovery'* ]] || \
       [[ "$text" == *'Windows failed to start'* ]] || \
       [[ "$text" == *'Start Windows Normally'* ]]; then
        screen_state='recovery-menu'
        if [[ "$vista_last_screen_state" != "$screen_state" ]]; then
            (( vista_recovery_menu_visits += 1 ))
            if (( vista_recovery_menu_visits > 1 )); then
                abort_vista_boot "$phase" \
                    'boot loop detected: Windows Error Recovery appeared again'
            fi
        fi
    fi
    vista_last_screen_state=$screen_state
}

qmp_call() {
    local request=$1
    printf '{"execute":"qmp_capabilities"}\n%s\n' "$request" | \
        nc -w 5 -U "$qmp_socket"
}

capture_vista_screen() {
    test -S "$qmp_socket" || return 1
    rm -f "$screen_png"
    qmp_call "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$screen_png\",\"format\":\"png\"}}" >/dev/null
}

vista_screen_text() {
    capture_vista_screen || return 1
    tesseract "$screen_png" stdout --psm 11 -l eng 2>/dev/null || true
}

vista_screen_has_visible_pixels() {
    # An attached scanout can still contain only zeroed framebuffer bytes.
    # Require a small whole-frame luminance floor so a cursor or a few test-
    # signing watermark pixels cannot turn the black-primary failure green.
    local mean
    mean=$(magick "$screen_png" -colorspace RGB -format '%[fx:mean]' info: 2>/dev/null) || \
        return 1
    awk -v mean="$mean" 'BEGIN { exit !(mean > 0.005) }'
}

wait_for_vista_screen_marker() {
    local marker=$1
    local timeout=$2
    local text='' upper_text upper_marker=${marker:u}
    for (( attempt = 1; attempt <= timeout; ++attempt )); do
        text=$(vista_screen_text || true)
        guard_vista_runtime "waiting for guest marker $marker" "$text"
        if (( attempt % 5 == 1 )); then
            guard_checked_kd_bugcheck "waiting for guest marker $marker"
        fi
        upper_text=${text:u}
        if [[ "$upper_text" == *"$upper_marker"* ]]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

vista_qemu_pid() {
    ps -axo pid=,command= | awk -v binary="$qemu_binary" -v disk="$vista_disk" \
        '$2 == binary && index($0, disk) != 0 { print $1 }'
}

ensure_checked_kd_pacer() {
    if (( kd_pacer_ready )); then
        return 0
    fi

    # Restart the exact named transport once per unified-script run.  This
    # guarantees that edits to KdTcpPacer.cs take effect and prevents a stale
    # process from carrying serial bytes across separate Vista boots.
    screen -S "$kd_pacer_screen" -X quit >/dev/null 2>&1 || true
    for attempt in {1..30}; do
        if ! lsof -nP -t -iTCP:2021 -sTCP:LISTEN >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done

    # A detached screen server can disappear while its dotnet child remains
    # alive.  Do not accept that orphan as a fresh transport: it retains KD
    # framing bytes from the preceding boot and can trap the next checked
    # kernel in an endless RESET handshake.  Validate the listener by both
    # executable and working directory before replacing that exact process.
    local stale_pacer_pid stale_pacer_command stale_pacer_cwd
    stale_pacer_pid=$(lsof -nP -t -iTCP:2021 -sTCP:LISTEN 2>/dev/null || true)
    if test -n "$stale_pacer_pid"; then
        [[ "$stale_pacer_pid" != *$'\n'* ]] || \
            fail "more than one process is listening on checked-KD TCP 2021"
        stale_pacer_command=$(ps -p "$stale_pacer_pid" -o command= 2>/dev/null || true)
        stale_pacer_cwd=$(lsof -a -p "$stale_pacer_pid" -d cwd -Fn 2>/dev/null | \
            sed -n 's/^n//p' | head -1)
        if [[ "$stale_pacer_command" != *KdTcpPacer* ]] || \
           [[ "$stale_pacer_cwd" != "$workspace" ]]; then
            fail "TCP 2021 is occupied by an unrelated process: $stale_pacer_command"
        fi
        log "replacing an orphaned checked-Vista KD transport"
        kill -TERM "$stale_pacer_pid" 2>/dev/null || true
        for attempt in {1..30}; do
            kill -0 "$stale_pacer_pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$stale_pacer_pid" 2>/dev/null; then
            kill -KILL "$stale_pacer_pid" 2>/dev/null || true
        fi
    fi

    # screen(1) returns 1 even when it successfully lists detached sessions.
    # Capture its text separately so pipefail cannot turn that into "missing".
    local screen_sessions
    screen_sessions=$(screen -list 2>/dev/null || true)
    if ! printf '%s\n' "$screen_sessions" | \
         rg -q "[.]${kd_pacer_screen}[[:space:]]"; then
        log "starting the checked-Vista KD transport"
        : > "$kd_pacer_log"
        screen -dmS "$kd_pacer_screen" zsh -lc \
            "cd ${(q)workspace} && exec dotnet run test-artifacts/KdTcpPacer.cs >> ${(q)kd_pacer_log} 2>&1"
    fi

    local listener_pid
    for attempt in {1..30}; do
        listener_pid=$(lsof -nP -t -iTCP:2021 -sTCP:LISTEN 2>/dev/null || true)
        if test -n "$listener_pid"; then
            if ps -p "$listener_pid" -o command= | rg -q 'KdTcpPacer'; then
                kd_pacer_ready=1
                return 0
            fi
            fail "TCP 2021 is occupied by a process other than the checked-Vista KD transport"
        fi
        sleep 1
    done
    fail "the checked-Vista KD transport did not listen on TCP 2021; inspect $kd_pacer_log"
}

stop_checked_kd() {
    prlctl exec "$windows_vm" cmd.exe /d /c \
        'taskkill /im kd.exe /f >nul 2>nul & taskkill /im KdController.exe /f >nul 2>nul & exit /b 0' \
        >/dev/null || fail "could not stop the previous checked kernel debugger"
}

launch_checked_kd() {
    vista_checked_kd_active=0
    if ! prlctl list -a --json | rg -q '"name": "Windows 11"'; then
        fail "the Windows 11 KD VM is unavailable"
    fi

    ensure_checked_kd_pacer
    log "attaching the checked kernel debugger from Windows 11"
    stop_checked_kd
    # Keep the controller in the Windows VM synchronized with this repository.
    # The task runs as SYSTEM, which cannot see the Parallels Y: share itself.
    prlctl exec "$windows_vm" --current-user cmd.exe /d /c \
        'copy /y Y:\test-artifacts\KdController.cs C:\VistaSigning\KdController.cs >nul & copy /y Y:\test-artifacts\KdSend.cs C:\VistaSigning\KdSend.cs >nul' \
        >/dev/null || fail "could not update the checked-KD controller in Windows 11"
    prlctl exec "$windows_vm" cmd.exe /d /c \
        'del /q C:\VistaSigning\vista-kd-controller.log C:\VistaSigning\vista-kd.commands >nul 2>nul & exit /b 0' \
        >/dev/null || fail "could not clear the previous checked kernel-debugger state"
    vista_allowed_kd_breaks=0
    vista_allowed_kd_display_errors=0
    vista_allowed_kd_prompts=0
    vista_allowed_taskeng_assertions=0
    prlctl exec "$windows_vm" schtasks.exe /create \
        /tn VistaKdControllerTask /sc ONCE /st 23:59 \
        /tr 'C:\PROGRA~1\dotnet\dotnet.exe run C:\VistaSigning\KdController.cs' \
        /ru SYSTEM /f >/dev/null || \
        fail "could not register the checked kernel-debugger task"
    prlctl exec "$windows_vm" schtasks.exe /run \
        /tn VistaKdControllerTask >/dev/null || \
        fail "could not start the checked kernel debugger in Windows 11"

    # KdController removes stale command/capture files before it starts kd.exe.
    # First wait until the new controller has published its PID.
    local controller_ready=0
    for attempt in {1..60}; do
        if prlctl exec "$windows_vm" cmd.exe /d /c \
            'findstr /c:"started kd.exe PID" C:\VistaSigning\vista-kd-controller.log' \
            >/dev/null 2>&1; then
            controller_ready=1
            break
        fi
        sleep 1
    done
    (( controller_ready )) || \
        fail "the Windows 11 checked kernel debugger did not start"
    vista_checked_kd_active=1
}

send_checked_kd_command() {
    local command=$1
    local kd_pid
    kd_pid=$(prlctl exec "$windows_vm" cmd.exe /d /c \
        'type C:\VistaSigning\vista-kd-controller.log' 2>/dev/null | \
        sed -n 's/.*started kd[.]exe PID \([0-9][0-9]*\).*/\1/p' | \
        tail -1 | tr -d '\r')
    [[ "$kd_pid" == <-> ]] || return 1
    prlctl exec "$windows_vm" cmd.exe /d /c \
        "\"C:\Program Files\dotnet\dotnet.exe\" run C:\VistaSigning\KdSend.cs $kd_pid \"$command\"" \
        >/dev/null
}

continue_checked_kd_at_initial_break() {
    # Do not infer readiness merely because kd.exe exists.  Wait until KD and
    # checked Vista have completed their RESET handshake.  KdController does
    # not request an initial break; after this marker the target is running and
    # any later breakpoint is a real checked-kernel failure.
    # pnputil/devcon can take several minutes in Safe Mode under single-core
    # TCG. KD cannot synchronize until that install finishes and reboots into
    # the normal checked kernel, so keep this finite but large enough for the
    # measured slow path.
    local screen_text=''
    for attempt in {1..900}; do
        if prlctl exec "$windows_vm" cmd.exe /d /c \
            'findstr /c:"Target synchronized successfully" C:\VistaSigning\vista-kd-capture.log' \
            >/dev/null 2>&1; then
            log "checked KD is synchronized and Vista is running"
            return 0
        fi
        if (( attempt % 3 == 1 )); then
            screen_text=$(vista_screen_text || true)
            guard_vista_runtime 'waiting for checked-KD initial break' "$screen_text"
            guard_checked_kd_bugcheck 'waiting for checked-KD initial break'
        fi
        # The TCP pacer intentionally survives guest reboots.  kd.exe does
        # not always survive the same RESET/reconnect sequence: it can leave
        # its socket while remaining stuck at "Waiting to reconnect".  Give
        # the installer time to reboot first, then replace only the Windows
        # debugger/controller.  QEMU and the Vista disk remain untouched.
        if (( attempt == 120 || attempt == 300 )); then
            log "checked KD has no initial-break prompt yet; refreshing its Windows 11 connection"
            launch_checked_kd
        fi
        sleep 1
    done
    fail "the checked kernel debugger did not reach Vista's initial break"
}

continue_known_legacy_rotation_break() {
    local break_ready=0 capture_ready=0 debug_log=''
    local expected_kmd_date='' expected_kmd_time='' expected_kmd_banner=''

    (( vista_checked_kd_active )) || return 1

    if ! prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"Break to debug, Ignore (bi)?" C:\VistaSigning\vista-kd-capture.log' \
        >/dev/null 2>&1; then
        if ! prlctl exec "$windows_vm" cmd.exe /d /c \
            'findstr /i /c:"Display Driver Model noncritical error detected" C:\VistaSigning\vista-kd-capture.log' \
            >/dev/null 2>&1; then
            return 1
        fi
    fi
    if (( vista_allowed_kd_breaks != 0 || vista_allowed_kd_prompts != 0 )); then
        return 1
    fi

    # The already-installed 7.14.1.3769 image presents one checked-kernel
    # DbgPrompt before StartDevice, without the explanatory DDM line.  It is
    # being replaced in this recovery session.  Ignore exactly one prompt only
    # when debugcon proves that exact pre-update binary is loaded.  A second
    # prompt, any other image, and every final accelerated boot remain fatal.
    if ! prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"Display Driver Model noncritical error detected" C:\VistaSigning\vista-kd-capture.log' \
        >/dev/null 2>&1; then
        if test -f "$debug_pointer"; then
            debug_log=$(<"$debug_pointer")
        fi
        # The diagnostic KMD currently installed on the image can emit the
        # same checked-DXG prompt before Safe Mode reaches the updater. Derive
        # its build marker from the package whose SHA-256 manifest was just
        # verified; a hard-coded __TIME__ turns every fresh build into an
        # unrelated failure. Capture its stack and continue this recovery
        # boot; final accelerated boots still reject any prompt.
        if test -s "$package_dir/viogpu3d.sys"; then
            expected_kmd_date=$(strings -a "$package_dir/viogpu3d.sys" | \
                rg -m1 '^(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9][0-9] 20[0-9][0-9]$' || true)
            expected_kmd_time=$(strings -a "$package_dir/viogpu3d.sys" | \
                rg -m1 '^[0-2][0-9]:[0-5][0-9]:[0-5][0-9]$' || true)
            if test -n "$expected_kmd_date" && test -n "$expected_kmd_time"; then
                expected_kmd_banner="VIOGPU FULL build on on $expected_kmd_date $expected_kmd_time"
            fi
        fi
        if test -n "$debug_log" && test -f "$debug_log" && \
           { { test -n "$expected_kmd_banner" && \
               rg -a -Fq -- "$expected_kmd_banner" "$debug_log"; } || \
             rg -a -Fq -- \
               'VIOGPU FULL build on on Aug 13 2026 10:42:19' "$debug_log"; }; then
            send_checked_kd_command b || abort_vista_boot \
                'inspecting the current-KMD recovery prompt' \
                'could not select break at the current-KMD prompt'
            for attempt in {1..30}; do
                if prlctl exec "$windows_vm" cmd.exe /d /c \
                    'findstr /i /c:"Break instruction exception" C:\VistaSigning\vista-kd-capture.log' \
                    >/dev/null 2>&1; then
                    break_ready=1
                    break
                fi
                sleep 1
            done
            (( break_ready )) || abort_vista_boot \
                'inspecting the current-KMD recovery prompt' \
                'checked KD did not enter the requested current-KMD break'
            send_checked_kd_command kv || abort_vista_boot \
                'inspecting the current-KMD recovery prompt' \
                'could not request the current-KMD stack'
            send_checked_kd_command '.echo VISTA_CURRENT_KMD_RECOVERY_STACK_DONE' || \
                abort_vista_boot 'inspecting the current-KMD recovery prompt' \
                'could not queue the current-KMD stack marker'
            for attempt in {1..60}; do
                if prlctl exec "$windows_vm" cmd.exe /d /c \
                    'findstr /c:"VISTA_CURRENT_KMD_RECOVERY_STACK_DONE" C:\VistaSigning\vista-kd-capture.log' \
                    >/dev/null 2>&1; then
                    capture_ready=1
                    break
                fi
                sleep 1
            done
            (( capture_ready )) || abort_vista_boot \
                'inspecting the current-KMD recovery prompt' \
                'checked KD did not return the current-KMD stack'
            vista_allowed_kd_breaks=1
            vista_allowed_kd_prompts=1
            send_checked_kd_command g || abort_vista_boot \
                'continuing the current-KMD recovery prompt' \
                'could not continue the current-KMD stop'
            log "captured and continued the manifest-verified current-KMD recovery prompt"
            sleep 1
            return 0
        fi
        if test -z "$debug_log" || test ! -f "$debug_log" || \
           ! rg -a -q 'VIOGPU FULL build on on Aug 12 2026 (17:25:34|21:35:38)' "$debug_log"; then
            abort_vista_boot 'inspecting the pre-update recovery prompt' \
                'checked KD stopped on a prompt not produced by the known installed KMD'
        fi
        vista_allowed_kd_prompts=1
        send_checked_kd_command i || abort_vista_boot \
            'continuing the pre-update recovery prompt' \
            'could not ignore the verified installed-KMD prompt'
        log "ignored the single verified checked prompt from the pre-update KMD"
        sleep 2
        return 0
    fi

    # Select "break" at the checked dxgkrnl prompt, then prove the exact stack
    # before continuing.  Never ignore an unidentified display-model error and
    # never apply this exception to the final accelerated boot.
    send_checked_kd_command b || abort_vista_boot \
        'inspecting the legacy recovery breakpoint' \
        'could not select break at the checked display-model prompt'
    for attempt in {1..30}; do
        if prlctl exec "$windows_vm" cmd.exe /d /c \
            'findstr /i /c:"Break instruction exception" C:\VistaSigning\vista-kd-capture.log' \
            >/dev/null 2>&1; then
            break_ready=1
            break
        fi
        sleep 1
    done
    (( break_ready )) || abort_vista_boot \
        'inspecting the legacy recovery breakpoint' \
        'checked KD did not enter the requested diagnostic break'
    send_checked_kd_command kv || abort_vista_boot \
        'inspecting the legacy recovery breakpoint' \
        'could not request the checked-KD stack'
    send_checked_kd_command '.echo VISTA_LEGACY_ROTATION_STACK_DONE' || \
        abort_vista_boot 'inspecting the legacy recovery breakpoint' \
        'could not queue the checked-KD stack marker'
    for attempt in {1..60}; do
        if prlctl exec "$windows_vm" cmd.exe /d /c \
            'findstr /c:"VISTA_LEGACY_ROTATION_STACK_DONE" C:\VistaSigning\vista-kd-capture.log' \
            >/dev/null 2>&1; then
            capture_ready=1
            break
        fi
        sleep 1
    done
    (( capture_ready )) || abort_vista_boot \
        'inspecting the legacy recovery breakpoint' \
        'checked KD did not return the stopped stack'
    if ! prlctl exec "$windows_vm" cmd.exe /d /c \
        'findstr /i /c:"dxgkrnl!DXGMONITOR::DetermineMonitorConnectivityInfo" C:\VistaSigning\vista-kd-capture.log' \
        >/dev/null 2>&1; then
        abort_vista_boot 'inspecting the legacy recovery breakpoint' \
            'the stopped stack is not the known pre-fix StatusRotation assertion'
    fi

    vista_allowed_kd_breaks=1
    vista_allowed_kd_display_errors=1
    vista_allowed_kd_prompts=1
    send_checked_kd_command g || abort_vista_boot \
        'continuing the legacy recovery breakpoint' \
        'could not continue the verified checked-KD stop'
    log "continued the verified pre-fix StatusRotation assertion so the updater can replace the old KMD"
    sleep 1
    return 0
}

stop_vista_vm() {
    local pid
    pid=$(vista_qemu_pid)
    test -n "$pid" || return 0
    if [[ "$pid" == *$'\n'* ]]; then
        fail "more than one QEMU process is using $vista_disk"
    fi

    log "stopping the current Vista test boot"
    if test -S "$qmp_socket"; then
        qmp_call '{"execute":"system_powerdown"}' >/dev/null || true
    fi
    for attempt in {1..45}; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 1
    done

    # A broken display miniport can prevent Vista from completing an ACPI
    # shutdown.  The PID above is resolved against both the exact QEMU binary
    # and the exact test disk before it is terminated.
    log "Vista did not finish shutdown; terminating the exact test QEMU process"
    kill -TERM "$pid"
    for attempt in {1..20}; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 1
    done
    fail "QEMU process $pid did not terminate"
}

start_vista_vm() {
    local boot_mode=${1:-accelerated}
    if [[ "$boot_mode" != safe && "$boot_mode" != recovery && \
          "$boot_mode" != accelerated ]]; then
        fail "unknown Vista boot mode: $boot_mode"
    fi
    test -x "$qemu_binary" || fail "missing Triton QEMU binary: $qemu_binary"
    test -s "$npt_backend_library" || \
        fail "missing Neptune DXMT backend: $npt_backend_library"
    test -x "$npt_render_server" || \
        fail "missing Neptune render server: $npt_render_server"
    test -s "$vista_disk" || fail "missing Vista test disk: $vista_disk"

    # A successful validation deliberately leaves the accelerated guest up.
    # Its QEMU child can outlive the detached screen server, so closing the
    # named screen alone is not a reliable mode transition.  Stop only the
    # process resolved from this exact QEMU binary and this exact qcow2 before
    # unlinking its monitor sockets or attempting to reacquire the disk lock.
    if test -n "$(vista_qemu_pid)"; then
        log "stopping the existing exact Vista QEMU before $boot_mode relaunch"
        stop_vista_vm
    fi
    screen -S "$qemu_screen" -X quit >/dev/null 2>&1 || true
    rm -f "$qmp_socket" "$hmp_socket"

    local debug_log qemu_stderr
    debug_log=$(mktemp /tmp/vista-viogpu-unified-debugcon.XXXXXX)
    qemu_stderr=$(mktemp /tmp/vista-viogpu-unified-qemu.XXXXXX)
    printf '%s\n' "$debug_log" > "$debug_pointer"
    printf '%s\n' "$qemu_stderr" > "$qemu_log_pointer"
    vista_recovery_menu_visits=0
    vista_last_screen_state=''
    vista_allowed_driver_entries=1

    local -a display_devices deployment_media
    if [[ "$boot_mode" == safe ]]; then
        # Safe Mode package replacement needs the virtio PCI function present
        # so devcon can update it, but Vista does not start its display stack.
        display_devices=(
            -vga std
            -device 'virtio-gpu-gl-pci,hostmem=1G,max_hostmem=1G,blob=true,neptune=true'
        )
    elif [[ "$boot_mode" == recovery ]]; then
        # Probe-only staging needs no display-device update.  Do not attach
        # the known-bad virtio display function here: doing so lets normal
        # PnP load Triton beside Standard VGA and defeats the purpose of a
        # driver-independent recovery desktop.
        display_devices=(
            -vga std
        )
    else
        display_devices=(
            -vga none
            -device 'virtio-vga-gl,hostmem=1G,max_hostmem=1G,blob=true,neptune=true'
        )
    fi
    if [[ "$boot_mode" == safe || \
          ( "$boot_mode" == recovery && "$probe_only" == 1 ) ]]; then
        test -s "$vista_deploy_iso" || \
            fail "missing Vista deployment ISO: $vista_deploy_iso"
        deployment_media=(
            -drive "file=$vista_deploy_iso,format=raw,media=cdrom,readonly=on"
        )
    fi

    local -a qemu_command=(
        "$qemu_binary"
        -name "vista-aero-$boot_mode"
        -accel tcg
        -machine q35
        -m 768M
        -smp 1
        -drive "file=$vista_disk,format=qcow2,if=ide"
        "${display_devices[@]}"
        "${deployment_media[@]}"
        # RTL8139 is available in this image during a normal recovery boot.
        # Safe Mode loads neither its RTL8139 nor e1000 service.
        -nic 'user,model=rtl8139'
        -device 'qemu-xhci,id=vista_xhci'
        -device 'usb-tablet,id=vista_tablet'
        -display 'cocoa,gl=on'
        -monitor "unix:$hmp_socket,server,nowait"
        -qmp "unix:$qmp_socket,server=on,wait=off"
        -serial 'tcp:0.0.0.0:2020,server=on,wait=off'
        # A guest bugcheck must terminate this one boot.  The script performs
        # every intentional mode transition with shutdown plus a fresh QEMU
        # launch, so allowing automatic guest resets can only hide a crash and
        # turn it into a destructive boot loop.
        -no-reboot
        -debugcon "file:$debug_log"
        -global 'isa-debugcon.iobase=0xe9'
    )

    local qemu_shell_command
    qemu_shell_command=${(j: :)${(q)qemu_command}}
    log "starting Vista from the original qcow2 ($boot_mode mode)"
    # The arm64 Neptune worker loads its D3D11/DXGI provider at run time.
    # An unqualified libdxmt-native.dylib is not in macOS's default dlopen
    # search path, so the renderer can create rings while every D3D11 device
    # request still returns E_FAIL.  Pin the backend and worker from the same
    # host-triton staging tree as QEMU's virglrenderer dependency.
    screen -dmS "$qemu_screen" zsh -lc \
        "NPT_BACKEND=dxmt NPT_D3D11_LIBRARY_PATH=${(q)npt_backend_library} NPT_DXGI_LIBRARY_PATH=${(q)npt_backend_library} RENDER_SERVER_EXEC_PATH=${(q)npt_render_server} exec $qemu_shell_command 2>${(q)qemu_stderr}"
    for attempt in {1..30}; do
        if test -S "$qmp_socket" && test -S "$hmp_socket"; then
            if [[ "$boot_mode" == safe ]]; then
                # Queue F8 repeatedly before OCR starts so the installation
                # Safe Mode boot is deterministic.
                for key_attempt in {1..60}; do
                    printf 'sendkey f8\n'
                    sleep 0.2
                done | nc -w 20 -U "$hmp_socket" >/dev/null || true
            fi
            return 0
        fi
        sleep 1
    done
    fail "Vista QMP did not start; inspect $qemu_stderr"
}

build_vista_deploy_iso() {
    local media_stage
    command -v hdiutil >/dev/null || fail "hdiutil is required to build the Vista deployment ISO"
    media_stage=$(mktemp -d /tmp/vista-driver-deploy-stage.XXXXXX)
    mkdir -p "$media_stage/$package_name_x64"
    cp -f "$transfer_root/vista-install-current-driver.cmd" "$media_stage/"
    cp -f "$package_dir"/* "$media_stage/$package_name_x64/"
    cp -f "$transfer_root/triton9_runtime_probe_x64.exe" "$media_stage/"
    cp -f "$workspace/test-artifacts/vista-stage-d3d9-probe.cmd" "$media_stage/"
    perl -pi -e 's/\r?\n\z/\r\n/' \
        "$media_stage/vista-stage-d3d9-probe.cmd"
    rm -f "$vista_deploy_iso"
    if ! hdiutil makehybrid -quiet -iso -joliet \
        -default-volume-name VISTA_DRIVER \
        -o "$vista_deploy_iso" "$media_stage"; then
        rm -rf -- "$media_stage"
        fail "could not build the Vista Safe Mode deployment ISO"
    fi
    rm -rf -- "$media_stage"
    test -s "$vista_deploy_iso" || fail "the Vista deployment ISO is empty"
}

wait_for_safe_mode() {
    local request_boot_menu=$1
    local menu_selected=0
    local safe_ticks=0
    local autocheck_complete_ticks=0
    local autocheck_reset_sent=0
    local text='' upper_text=''

    log "waiting for the Vista Safe Mode administrator command prompt"
    # This image contains a large OEM driver store.  A Safe Mode boot under
    # single-core TCG can spend more than four minutes enumerating it before
    # Explorer appears, while the CPU and loading list are still progressing.
    for attempt in {1..600}; do
        text=$(vista_screen_text || true)
        upper_text=${text:u}
        guard_vista_runtime 'waiting for Vista Safe Mode' "$text"
        # The pre-update display miniport can stop at its checked
        # StatusRotation diagnostic between OCR frames. Inspect the KD prompt
        # on every loop before the strict guard, so a driver-generated prompt
        # cannot be misclassified as a recovery/login failure.
        continue_known_legacy_rotation_break || true
        if (( attempt % 5 == 1 )); then
            guard_checked_kd_bugcheck 'waiting for Vista Safe Mode'
        fi

        # A stale kd.exe connection can complete RESET synchronization but
        # loop on RESEND packets forever, leaving the target black before any
        # DbgKdContinue.  Refresh only the Windows debugger/controller; keep
        # QEMU, its serial transport, and the Vista disk running.
        if (( vista_checked_kd_active && attempt <= 300 && attempt % 60 == 0 )) && \
           ! prlctl exec "$windows_vm" cmd.exe /d /c \
               'findstr /c:"DbgKdContinue(" C:\VistaSigning\vista-kd-capture.log' \
               >/dev/null 2>&1; then
            log "checked KD has not continued Safe Mode; refreshing its Windows 11 connection"
            launch_checked_kd
        fi

        if (( !menu_selected )) && \
           { [[ "$text" == *'Windows Error Recovery'* ]] || \
             [[ "$text" == *'Windows failed to start'* ]] || \
             [[ "$text" == *'Start Windows Normally'* ]] || \
             [[ "$text" == *'Windows did not shut down successfully'* ]] || \
             [[ "$text" == *'Mode with Networking'* ]]; }; then
            # The recovery menu defaults to Start Windows Normally.  Safe Mode
            # with Command Prompt is the entry immediately above it.  Boot it
            # directly so deployment does not depend on Explorer's unreliable
            # Safe Mode Run-dialog process launch.
            for key in up ret; do
                printf 'sendkey %s\n' "$key" | nc -w 5 -U "$hmp_socket" >/dev/null
                sleep 0.25
            done
            menu_selected=1
            safe_ticks=0
        elif (( !menu_selected )) && [[ "$text" == *'Advanced Boot Options'* ]]; then
            printf 'sendkey home\n' | nc -w 5 -U "$hmp_socket" >/dev/null
            sleep 0.25
            if [[ "$text" == *'Repair Your Computer'* ]]; then
                printf 'sendkey down\n' | nc -w 5 -U "$hmp_socket" >/dev/null
                sleep 0.25
            fi
            printf 'sendkey down\n' | nc -w 5 -U "$hmp_socket" >/dev/null
            sleep 0.25
            printf 'sendkey down\n' | nc -w 5 -U "$hmp_socket" >/dev/null
            sleep 0.25
            printf 'sendkey ret\n' | nc -w 5 -U "$hmp_socket" >/dev/null
            menu_selected=1
            safe_ticks=0
        elif [[ "$text" == *'Windows Activation'* ]]; then
            # This image shows the activation reminder before every Safe Mode
            # desktop.  The tested coordinate chooses Activate later.
            python3 "$workspace/scripts/qmpinput.py" click 220 290
            safe_ticks=0
        elif [[ "$text" == *'Help and Support'* ]] && \
             [[ "$text" != *'Administrator>'* ]]; then
            python3 "$workspace/scripts/qmpinput.py" combo alt f4
            sleep 2
            log "Vista Safe Mode desktop is ready"
            return 0
        elif [[ "$text" == *'Welcome Center'* ]] && \
             [[ "$text" != *'Administrator>'* ]]; then
            # The activation reminder can leave Welcome Center maximized and
            # unfocused.  Close its fixed 800x600 title-bar button directly.
            python3 "$workspace/scripts/qmpinput.py" click 736 18
            sleep 2
            log "Vista Safe Mode desktop is ready"
            return 0
        elif [[ "$text" == *'Administrator>'* ]] || \
             [[ "$text" == *'system32>'* ]]; then
            log "Vista Safe Mode administrator command prompt is already ready"
            return 0
        elif [[ "$upper_text" == *'WINDOWS HAS FINISHED CHECKING THE DISK'* ]]; then
            # Autochk has completed, but this checked Vista image can leave
            # the completed text mode framebuffer visible indefinitely rather
            # than continuing to the Safe Mode loader.  It is safe to reset
            # only after the explicit completion line, and only once: the
            # filesystem has already been closed cleanly and the existing
            # boot-menu loop will reselect Safe Mode after the QMP reset.
            (( autocheck_complete_ticks += 1 ))
            if (( !autocheck_reset_sent && autocheck_complete_ticks >= 15 )); then
                log "Autochk completed but did not continue; resetting Vista once through QMP"
                printf '{"execute":"qmp_capabilities"}\n{"execute":"system_reset"}\n' | \
                    nc -w 5 -U "$qmp_socket" >/dev/null || \
                    fail "could not reset Vista after completed Autochk"
                autocheck_reset_sent=1
                menu_selected=0
                safe_ticks=0
                autocheck_complete_ticks=0
            fi
        elif [[ "$text" == *'Safe Mode'* ]] && \
             [[ "$text" != *'Loading Windows Files'* ]] && \
             [[ "$text" != *'Welcome'* ]] && \
             [[ "$text" == *'Recycle Bin'* ]]; then
            (( safe_ticks += 1 ))
            # Safe Mode paints its corner labels before Explorer is ready.
            # Require the shell to remain stable long enough for Vista's
            # automatic Safe Mode Help window to appear.  The branch above
            # closes that window before any deployment input is sent.
            if (( safe_ticks >= 30 )); then
                log "Vista Safe Mode desktop is ready"
                return 0
            fi
        else
            safe_ticks=0
            autocheck_complete_ticks=0
        fi

        # A checked-DXG recovery prompt can pause the kernel for several
        # minutes before bootmgr accepts F8. Keep requesting the boot menu
        # until its OCR state is observed instead of expiring this input on a
        # wall-clock deadline while the target is stopped in KD.
        if (( request_boot_menu && !menu_selected )); then
            python3 "$workspace/scripts/qmpinput.py" key f8 || true
        fi
        sleep 1
    done

    capture_vista_screen || true
    fail "Vista did not reach Safe Mode; inspect $screen_png"
}

open_vista_command_prompt() {
    local text=''

    text=$(vista_screen_text || true)
    guard_vista_runtime 'opening the Vista administrator prompt' "$text"
    if [[ "$text" == *'Administrator>'* ]] || \
       [[ "$text" == *'system32>'* ]]; then
        log "Vista administrator command prompt is ready"
        return 0
    fi

    # Do not close windows solely because OCR can see their text: Help and a
    # stale Run dialog can be behind an already-focused command prompt. That
    # previously made Alt+F4 close the prompt and turned deployment into a
    # focus-dependent loop. Show the desktop first, then open one known Run
    # dialog from a deterministic focus state.
    printf 'sendkey meta_l-d\n' | nc -w 5 -U "$hmp_socket" >/dev/null || true
    sleep 2

    for attempt in {1..6}; do
        printf 'sendkey esc\n' | nc -w 5 -U "$hmp_socket" >/dev/null || true
        # Send Win+R as one hardware chord.  Vista intermittently interprets
        # separately delivered QMP modifier events as only the Windows key.
        printf 'sendkey meta_l-r\n' | nc -w 5 -U "$hmp_socket" >/dev/null
        sleep 2
        text=$(vista_screen_text || true)
        guard_vista_runtime 'opening the Vista Run dialog' "$text"
        if [[ "$text" != *'Run'* ]]; then
            continue
        fi

        # Run preserves its last command.  Clear it before entering the
        # installer shell.  HMP's atomic chord is reliable on Vista, while
        # separate QMP modifier messages can degrade into plain 'a'.
        printf 'sendkey ctrl-a\n' | nc -w 5 -U "$hmp_socket" >/dev/null
        python3 "$workspace/scripts/qmpinput.py" key backspace
        python3 "$workspace/scripts/qmpinput.py" type 'cmd.exe'
        # Submit through QMP/HMP only.  Host GUI/accessibility automation is
        # deliberately not used on macOS; QMP typing and the absolute tablet
        # click are sufficient and keep the test reproducible.
        python3 "$workspace/scripts/qmpinput.py" key ret || \
            python3 "$workspace/scripts/qmpinput.py" click 170 527
        for prompt_attempt in {1..60}; do
            sleep 1
            text=$(vista_screen_text || true)
            guard_vista_runtime 'waiting for the Vista administrator prompt' "$text"
            if [[ "$text" == *'Help and Support'* ]]; then
                # Safe Mode Help is launched asynchronously and can steal
                # focus after Run has already accepted cmd.exe.  Close Help,
                # then submit the still-open Run dialog again.
                printf 'sendkey alt-f4\n' | nc -w 5 -U "$hmp_socket" >/dev/null || true
                sleep 1
                printf 'sendkey ret\n' | nc -w 5 -U "$hmp_socket" >/dev/null || true
                continue
            fi
            if [[ "$text" == *'Administrator>'* ]] || \
               [[ "$text" == *'system32>'* ]]; then
                log "Vista administrator command prompt is ready"
                return 0
            fi
        done

        # Close the one failed Run or command window before retrying.  Do not
        # accumulate background prompts that can steal deployment input.
        printf 'sendkey alt-f4\n' | nc -w 5 -U "$hmp_socket" >/dev/null || true
        sleep 2
    done

    capture_vista_screen || true
    fail "could not open the Vista administrator command prompt; inspect $screen_png"
}

ensure_vista_safe_mode() {
    command -v tesseract >/dev/null || fail "tesseract is required for unattended Vista boot-state detection"
    command -v sips >/dev/null || fail "sips is required for unattended Vista screenshots"
    command -v magick >/dev/null || fail "ImageMagick is required for the non-black Vista scanout gate"

    local pid text qemu_command
    pid=$(vista_qemu_pid)
    if test -n "$pid" && test -S "$qmp_socket"; then
        qemu_command=$(ps -p "$pid" -o command= 2>/dev/null || true)
        # Every launch mode uses the same networking device.  Safe Mode is
        # identified by its Standard VGA device and the read-only deployment
        # ISO.  Accepting an accelerated boot here can stage against the wrong
        # desktop and, worse, silently reuse an old probe executable.
        if ! printf '%s\n' "$qemu_command" | rg -Fq -- '-vga std' || \
           ! printf '%s\n' "$qemu_command" | rg -Fq -- "$vista_deploy_iso"; then
            log "restarting Vista with the scripted Safe Mode display and deployment ISO"
            stop_vista_vm
            start_vista_vm safe
            wait_for_safe_mode 1
            return 0
        fi
        text=$(vista_screen_text || true)
        guard_vista_runtime 'checking the existing Vista Safe Mode boot' "$text"
        if [[ "$text" == *'Safe Mode'* ]] || \
           [[ "$text" == *'Loading Windows Files'* ]] || \
           [[ "$text" == *'Welcome'* ]] || \
           [[ "$text" == *'Windows Activation'* ]]; then
            # Do not terminate an in-flight Safe Mode boot merely because
            # the corner labels are not painted on the Welcome screen yet.
            wait_for_safe_mode 0
            return 0
        fi
        stop_vista_vm
    elif test -n "$pid"; then
        stop_vista_vm
    fi

    start_vista_vm safe
    wait_for_safe_mode 1
}

wait_for_vista_recovery_desktop() {
    local text=''
    local desktop_ticks=0
    log "waiting for the normal-mode Standard VGA recovery desktop"
    for attempt in {1..600}; do
        text=$(vista_screen_text || true)
        guard_vista_runtime 'waiting for the Standard VGA recovery desktop' "$text"
        continue_known_legacy_rotation_break || true
        guard_checked_kd_bugcheck 'waiting for the Standard VGA recovery desktop'
        if [[ "$text" == *'Windows Error Recovery'* ]] || \
           [[ "$text" == *'Windows failed to start'* ]] || \
           [[ "$text" == *'Start Windows Normally'* ]]; then
            # The recovery-mode QEMU launch does not queue F8.  This menu is
            # only the consequence of the preceding forced recovery shutdown,
            # and its default/highlighted final row is Start Windows Normally.
            python3 "$workspace/scripts/qmpinput.py" key ret
            desktop_ticks=0
        elif [[ "$text" == *'Safe Mode'* ]]; then
            if (( probe_only )); then
                log "persistent safeboot selected Safe Mode; accepting it for probe-only staging"
                wait_for_safe_mode 0
                return 0
            fi
            fail "Vista remained in Safe Mode after the safeboot removal gate"
        elif [[ "$text" == *'Windows Activation'* ]]; then
            python3 "$workspace/scripts/qmpinput.py" click 220 290
            desktop_ticks=0
        elif [[ "$text" == *'Welcome'* ]] || \
             [[ "$text" == *'Please wait'* ]]; then
            desktop_ticks=0
        elif [[ "$text" == *'Recycle Bin'* ]]; then
            (( desktop_ticks += 1 ))
            if (( desktop_ticks >= 5 )); then
                log "Vista normal-mode recovery desktop is ready"
                return 0
            fi
        else
            desktop_ticks=0
        fi
        sleep 1
    done
    capture_vista_screen || true
    fail "Vista did not reach its normal-mode recovery desktop; inspect $screen_png"
}

enter_vista_recovery_install_mode() {
    open_vista_command_prompt
    python3 "$workspace/scripts/qmpinput.py" combo ctrl c
    sleep 2
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /debug on'
    python3 "$workspace/scripts/qmpinput.py" key ret
    sleep 2
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /dbgsettings serial debugport:1 baudrate:115200'
    python3 "$workspace/scripts/qmpinput.py" key ret
    sleep 2
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /deletevalue {current} safeboot'
    python3 "$workspace/scripts/qmpinput.py" key ret
    sleep 2
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /deletevalue {default} safeboot'
    python3 "$workspace/scripts/qmpinput.py" key ret
    sleep 2
    python3 "$workspace/scripts/qmpinput.py" type 'cls'
    python3 "$workspace/scripts/qmpinput.py" key ret
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /enum all | findstr /i safeboot'
    python3 "$workspace/scripts/qmpinput.py" key ret
    sleep 2
    python3 "$workspace/scripts/qmpinput.py" type \
        'if errorlevel 1 echo OKNORMAL^READYOK'
    python3 "$workspace/scripts/qmpinput.py" key ret
    wait_for_vista_screen_marker NORMALREADY 60 || \
        fail "Vista still has a persistent safeboot entry before recovery boot"
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit|findstr /i debug|findstr /i yes>nul&&echo OKDEBUG^READYOK'
    python3 "$workspace/scripts/qmpinput.py" key ret
    wait_for_vista_screen_marker DEBUGREADY 60 || \
        fail "Vista active boot entry did not enable kernel debugging"
    python3 "$workspace/scripts/qmpinput.py" type \
        'shutdown /s /f /t 0'
    python3 "$workspace/scripts/qmpinput.py" key ret
    sleep 10
    stop_vista_vm
    # This is a checked Vista image.  dxgkrnl can raise a diagnostic breakpoint
    # while it validates the miniport immediately after AddDevice.  With kernel
    # debugging disabled that breakpoint becomes a fatal 0x7e/0x80000003 before
    # StartDevice, so arm KD before the normal recovery boot as well as before
    # the final accelerated boot.
    launch_checked_kd
    start_vista_vm recovery
    continue_checked_kd_at_initial_break
    wait_for_vista_recovery_desktop
    open_vista_command_prompt
    python3 "$workspace/scripts/qmpinput.py" combo ctrl c
    python3 "$workspace/scripts/qmpinput.py" type \
        'cls & ipconfig | find "10.0.2." >nul && echo OKNETWORK^READYOK'
    python3 "$workspace/scripts/qmpinput.py" key ret
    wait_for_vista_screen_marker NETWORKREADY 60 || \
        fail "Vista recovery boot did not acquire the QEMU user-network address"
    log "Vista normal-mode recovery networking is ready"
}

ensure_windows_build_vm() {
    if ! prlctl list "$windows_vm" --info | rg -q '^State: running$'; then
        log "starting or resuming the Windows signing VM"
        prlctl start "$windows_vm" || return 1
    fi
    # SmartMount is intentionally disabled on this build VM. Restore only the
    # two configured project shares for this interactive build session.
    prlctl exec "$windows_vm" cmd.exe /d /c \
        'net use Y: /delete /y >nul 2>&1 & net use Z: /delete /y >nul 2>&1 & net use Y: \\psf\winvistachecked /persistent:no >nul && net use Z: \\psf\aaaaa /persistent:no >nul' \
        || return 1
}

run_windows_batch() {
    local batch_path=$1
    shift
    ensure_windows_build_vm || {
        log "Windows signing VM or transient project shares are unavailable"
        return 1
    }
    if ! prlctl exec "$windows_vm" cmd.exe /d /c "$batch_path" "$@"; then
        log "Windows batch failed: $batch_path"
        for build_log in \
            "$transfer_root/w11-vista-x64-virtiolib-build.log" \
            "$transfer_root/w11-vista-x64-kmd-build.log" \
            "$transfer_root/w11-vista-x86-virtiolib-build.log" \
            "$transfer_root/w11-vista-x86-kmd-build.log"; do
            if test -f "$build_log"; then
                printf '[vista-driver] tail of %s\n' "$build_log" >&2
                tail -80 "$build_log" >&2
            fi
        done
        return 1
    fi
}

run_vista_meson() {
    env PYTHONPATH="${vista_mako_path}${PYTHONPATH:+:$PYTHONPATH}" meson "$@"
}

generate_source_manifest() {
    local destination=$1
    local temporary
    temporary=$(mktemp /tmp/vista-unified-source.XXXXXX)

    (
        cd "$workspace"
        {
            rg --files \
                triton-kmd/VirtIO \
                triton-kmd/build \
                triton-kmd/viogpu/common \
                triton-kmd/viogpu/shared \
                triton-kmd/viogpu/viogpu3d \
                triton-umd/build-support \
                triton-umd/src/virtio/neptune \
                test-artifacts/viogpu3d-vista-serialtrace.vcxproj \
                test-artifacts/viogpu3d-vista-x64.rsp \
                test-artifacts/viogpu3d-vista-x86.rsp \
                test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d-diagnostic.inf \
                test-artifacts/windows11_build_vista_serialtrace_kmd.bat \
                test-artifacts/windows11_sign_vista_serialtrace_kmd.bat \
                test-artifacts/windows11_stage_vista_pnp_package.bat \
                test-artifacts/vista-install-current-driver.cmd \
                test-artifacts/vista-driver-deploy-service.c \
                test-artifacts/vista-driver-deploy-service.manifest \
                test-artifacts/vista-driver-deploy-service.rc \
                test-artifacts/VISTA_GUEST_DEPLOY.md \
                scripts/build_deploy_vista_driver.sh \
                scripts/build_vista_deploy_service.sh \
                scripts/capture_vista_passive_png.py \
                scripts/run_vista_guest_deploy.sh \
                scripts/stage_vista_deploy_media.sh \
                scripts/verify_aero_glass_pixels.py \
                aaaaa/vista-signing-transfer/triton-vista-unlazy-signing.cer
            printf '%s\n' \
                triton-umd/meson.build \
                triton-umd/meson.options
        } | LC_ALL=C sort -u | while IFS= read -r source_file; do
            test -f "$source_file" && shasum -a 256 "$source_file"
        done
    ) > "$temporary"

    mv -f "$temporary" "$destination"
}

setup_vista_umd() {
    local architecture=$1
    local build_dir=$2
    local cross_file="$umd_root/build-support/vista-$architecture.ini"
    local -a setup_options=(
        --cross-file "$cross_file"
        --buildtype debugoptimized
        -Dplatforms=windows
        -Dneptune=true
        -Dnpt_wine=false
        -Dnpt_umd=off
        -Dnpt_vista_d3d9=true
        -Dmin-windows-version=6
        -Dgallium-drivers=
        -Dvulkan-drivers=
        -Dopengl=false
        -Degl=disabled
        -Dglx=disabled
        -Dllvm=disabled
        -Dbuild-tests=false
    )

    if test -f "$build_dir/meson-private/coredata.dat"; then
        run_vista_meson setup --reconfigure "$build_dir" "$umd_root" \
            "${setup_options[@]}"
    elif test -d "$build_dir"; then
        run_vista_meson setup --wipe "$build_dir" "$umd_root" \
            "${setup_options[@]}"
    else
        run_vista_meson setup "$build_dir" "$umd_root" "${setup_options[@]}"
    fi
}

prepare_vista_umd_build() {
    if test -n "$vista_python_bin"; then
        return 0
    fi
    local python_candidate
    for python_candidate in \
        /opt/homebrew/opt/python@3.13/bin/python3.13 \
        /opt/homebrew/bin/python3 \
        /usr/bin/python3; do
        if test -x "$python_candidate" && \
           "$python_candidate" -c 'import mako' >/dev/null 2>&1; then
            vista_python_bin=$python_candidate
            break
        fi
    done
    test -n "$vista_python_bin" || \
        fail "no Python interpreter with the Mako module is available for Meson"
    vista_mako_path=$("$vista_python_bin" -c \
        'from pathlib import Path; import mako; print(Path(mako.__file__).resolve().parents[1])')
    test -d "$vista_mako_path" || fail "could not resolve the Mako module path"

    log "preparing the pinned Vista CRT"
    "$umd_root/build-support/bootstrap-vista-crt.sh"
}

build_vista_umds() {
    prepare_vista_umd_build

    log "building the native Vista x64 D3D9 UMD"
    setup_vista_umd x64 "$umd_build_x64"
    run_vista_meson compile -C "$umd_build_x64" \
        neptune_d3d9 triton9_abi_compile triton9_runtime_probe

    log "building the Vista x86 WoW64 D3D9 UMD"
    setup_vista_umd x86 "$umd_build_x86"
    run_vista_meson compile -C "$umd_build_x86" \
        neptune_d3d9 triton9_abi_compile triton9_runtime_probe

    local x64_dll="$umd_build_x64/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll"
    local x86_dll="$umd_build_x86/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll"
    local x64_probe="$umd_build_x64/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe"
    local x86_probe="$umd_build_x86/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe"
    "$umd_root/build-support/audit-vista-d3d9-pe.sh" x64 "$x64_dll"
    "$umd_root/build-support/audit-vista-d3d9-pe.sh" x86 "$x86_dll"

    mkdir -p "$umd_stage"
    cp -f "$x64_dll" "$umd_stage/neptune_d3d9.dll"
    cp -f "$x86_dll" "$umd_stage/neptune_d3d9_wow.dll"
    cp -f "$x64_probe" "$transfer_root/triton9_runtime_probe_x64.exe"
    cp -f "$x86_probe" "$transfer_root/triton9_runtime_probe_x86.exe"
}

build_vista_deploy_service() {
    log "building the Vista guest-owned deployment service"
    "$workspace/scripts/build_vista_deploy_service.sh"
}

build_vista_probes() {
    prepare_vista_umd_build

    log "building the standalone Vista x64 D3D9 probe"
    setup_vista_umd x64 "$umd_build_x64"
    run_vista_meson compile -C "$umd_build_x64" triton9_runtime_probe

    log "building the standalone Vista x86 D3D9 probe"
    setup_vista_umd x86 "$umd_build_x86"
    run_vista_meson compile -C "$umd_build_x86" triton9_runtime_probe

    local x64_probe="$umd_build_x64/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe"
    local x86_probe="$umd_build_x86/src/virtio/neptune/vista-d3d9/triton9_runtime_probe.exe"
    test -s "$x64_probe" || fail "the Vista x64 D3D9 probe was not built"
    test -s "$x86_probe" || fail "the Vista x86 D3D9 probe was not built"
    cp -f "$x64_probe" "$transfer_root/triton9_runtime_probe_x64.exe"
    cp -f "$x86_probe" "$transfer_root/triton9_runtime_probe_x86.exe"
}

audit_package() {
    local architecture=$1
    local package_dir=$2
    local inf_name
    if [[ "$architecture" == x64 ]]; then
        inf_name='viogpu3d-diagnostic.inf'
    else
        inf_name='viogpu3d.inf'
    fi
    python3 "$workspace/triton-kmd/viogpu/tools/check_vista_pe.py" \
        --kind kmd --arch "$architecture" "$package_dir/viogpu3d.sys"
    if [[ "$architecture" == x64 ]]; then
        python3 "$workspace/triton-kmd/viogpu/tools/check_vista_pe.py" \
            --kind umd --arch x64 "$package_dir/neptune_d3d9.dll"
        python3 "$workspace/triton-kmd/viogpu/tools/check_vista_pe.py" \
            --kind umd --arch x86 "$package_dir/neptune_d3d9_wow.dll"
        python3 "$workspace/triton-kmd/viogpu/tools/check_vista_pe.py" \
            --kind exe --arch x64 \
            --allow-import crypt32.dll --allow-import wintrust.dll \
            "$package_dir/triton-vista-deploy.exe"
    else
        python3 "$workspace/triton-kmd/viogpu/tools/check_vista_pe.py" \
            --kind umd --arch x86 "$package_dir/neptune_d3d9.dll"
    fi
    python3 "$workspace/triton-kmd/viogpu/tools/check_vista_inf.py" \
        --arch "$architecture" --package-dir "$package_dir" \
        "$package_dir/$inf_name"
}

audit_source_build_contract() {
    local kmd_project="$workspace/test-artifacts/viogpu3d-vista-serialtrace.vcxproj"
    local virtio_project="$workspace/triton-kmd/VirtIO/VirtioLib.vcxproj"
    local x86_response="$workspace/test-artifacts/viogpu3d-vista-x86.rsp"
    local renderer="$workspace/triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c"
    local ddi="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c"
    local format="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_format.c"
    local output="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c"
    local unsupported="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_unsupported.c"
    local state="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp"
    local shader="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp"
    local draw_contract="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_draw_contract.c"
    local draw_contract_test="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_draw_contract_test.c"
    local shader_token_contract="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader_token_contract.h"
    local shader_token_test="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shader_token_contract_test.c"
    local abi_test="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_abi_test.c"
    local runtime_probe="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c"
    local allocation="$workspace/triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp"
    local paging="$workspace/triton-kmd/viogpu/viogpu3d/driver.cpp"
    local vidpn="$workspace/triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp"
    local protocol="$workspace/triton-umd/src/virtio/neptune/neptune-protocol/npt_protocol_common_types.h"
    local deploy_service="$workspace/test-artifacts/vista-driver-deploy-service.c"
    local guest_launcher="$workspace/scripts/run_vista_guest_deploy.sh"
    local media_stager="$workspace/scripts/stage_vista_deploy_media.sh"
    local passive_capture="$workspace/scripts/capture_vista_passive_png.py"
    local fog_parameter_block
    local embedded_signature_block
    local secure_probe_line
    local uxsms_probe_line

    python3 "$workspace/scripts/audit_triton_host_fence_contract.py"

    # These settings are required even when the x64 build succeeds.  WDK 7
    # relies on /Gz for x86 kernel imports and callbacks, while x64 has one
    # calling convention and can hide the error.
    rg -Fq '<CallingConvention>StdCall</CallingConvention>' "$kmd_project" || \
        fail "the Vista x86 KMD project does not use the WDK 7 /Gz convention"
    rg -Fq '<CallingConvention>StdCall</CallingConvention>' "$virtio_project" || \
        fail "the Vista x86 VirtIO library does not use the WDK 7 /Gz convention"
    rg -Fq 'BufferOverflowK.lib' "$x86_response" || \
        fail "the Vista x86 KMD link response omits BufferOverflowK.lib"

    # D3D9 accepts X8R8G8B8 as a display mode.  It accepts A8R8G8B8 as a
    # render target, but the alpha format must not claim DISPLAYMODE or
    # 3DACCELERATION.  Reversing these rows can prevent HAL selection.
    rg -Fq '{ D3DDDIFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4, TRITON9_ARGB_OPS, FALSE }' \
        "$format" || fail "the Vista UMD alpha format has invalid display capabilities"
    rg -Fq '{ D3DDDIFMT_X8R8G8B8, DXGI_FORMAT_B8G8R8X8_UNORM, 4, TRITON9_DISPLAY_OPS, FALSE }' \
        "$format" || fail "the Vista UMD XRGB format is not the display-mode entry"
    rg -Fq '"an alpha format cannot be a display mode"' "$format" || \
        fail "the Vista UMD format invariants are not compile-time checked"
    # DWM's first private resource after its colour targets is D24S8. Keep
    # the public FORMATOP row, translated stencil state, and public probe in
    # lockstep so a future reduction cannot regress back to a late d3d9.dll
    # fault during composition startup.
    for required in \
        'D3DDDIFMT_D24S8, DXGI_FORMAT_D24_UNORM_S8_UINT, 4' \
        'caps->StencilCaps = triton9FormatLookup(D3DDDIFMT_D24S8)' \
        'D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO' \
        'D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR' \
        'D3DSTENCILCAPS_TWOSIDED' \
        'triton9MapStencilOperation' \
        'D3D11_STENCIL_OP_REPLACE'; do
        rg -Fq "$required" "$format" "$ddi" "$shader" \
            "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp" || \
            fail "the Vista UMD D24S8/stencil contract is incomplete: $required"
    done
    rg -Fq 'caps D3D9_1 stencil contract PASS' "$runtime_probe" || \
        fail 'the public Vista D3D9 probe does not gate the advertised stencil contract'
    public_probe_line=$(rg -n -F 'if (probeLaunchPublicDesktop(&child, &publicDesktopProcessId))' \
        "$runtime_probe" | tail -n 1 | cut -d: -f1)
    uxsms_probe_line=$(rg -n -F '(void)probeEnsureUxSmsRunning();' \
        "$runtime_probe" | tail -n 1 | cut -d: -f1)
    [[ -n "$public_probe_line" && -n "$uxsms_probe_line" && \
       "$public_probe_line" -lt "$uxsms_probe_line" ]] || \
        fail 'the Vista probe starts UxSms before the independent public D3D9 gate'
    rg -Fq 'AERO probe skipped            D3D9 gate failed' "$runtime_probe" || \
        fail 'the Vista probe does not stop compositor testing after a D3D9 failure'
    # Vista DWM tags its D24S8 depth target as DiscardRenderTarget as well as
    # ZBuffer.  That bit alone does not make a resource presentable: only a
    # colour render target may enter the exported swap-chain allocation path.
    rg -Fq '!resource->isPrimary && args->Flags.DiscardRenderTarget &&' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c" || \
        fail 'the Vista UMD wrongly treats a D24S8 discard depth target as a present allocation'

    # The DWM image and the Vista display primary have intentionally opposite
    # allocation shapes.  DWM renders into an exported host blob; Present
    # sends that image to the KMD's standard, non-blob primary.  Exporting
    # Primary itself makes the direct scanout condition unreachable and can
    # still leave the desktop in Aero Basic even when DWM API calls succeed.
    resource_source="$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c"
    kmd_allocation="$workspace/triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp"
    kmd_present="$workspace/triton-kmd/viogpu/viogpu3d/viogpu_device.cpp"
    for required in \
        'triton9AllocateStandardPrimary' \
        'allocation.Type = VIOGPU_RESOURCE_TYPE_3D;' \
        'allocation.Options3D.flags = VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY;' \
        'callback.hResource = NULL;' \
        'allocationInfo.VidPnSourceId = resource->vidPnSourceId;' \
        'resource->isPrimary || !resource->hostResource' \
        'options->primary = 0;' \
        'callback.hResource = resource->isShared ? resource->hRTResource : NULL;' \
        'TRITON9-OPEN-STANDARD-PRIMARY success' \
        'VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY'; do
        rg -Fq "$required" "$resource_source" \
            "$workspace/triton-umd/src/virtio/virtio-gpu/wddm_hw.h" \
            "$workspace/triton-kmd/viogpu/shared/viogpum.h" \
            "$kmd_allocation" || \
            fail "the Vista DWM/primary allocation split is incomplete: $required"
    done
    rg -Fq 'dst->IsPrimary() && !dst->IsBlob()' "$kmd_present" || \
        fail 'the KMD no longer requires a non-blob primary for direct DWM scanout'
    rg -Fq 'TRITON9-AERO API-PATH PASS VISUAL-UNVERIFIED' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" || \
        fail 'the Aero probe can still mistake DWM API success for visual glass'
    for required in \
        'PROBE_AERO_BACKDROP_CLASS' \
        'PROBE_AERO_STRIPE_WIDTH' \
        'PROBE_AERO_WINSAT_TIMEOUT_MS' \
        '2u * PROBE_AERO_TRANSITION_TIMEOUT_MS' \
        'PROBE_AERO_SERVICE_WAIT_MS' \
        'g_childMode' \
        'probeFileContainsText' \
        'probeRelayFileGrowth' \
        'Do not call probeUploadTo here' \
        'probeLaunchPublicDesktop' \
        'processId = probeFindWinlogon(sessionId);' \
        'processId = probeFindProcessInSession(sessionId, "explorer.exe");' \
        'startup.lpDesktop = (LPSTR)"winsta0\\Winlogon";' \
        'startup.lpDesktop = (LPSTR)"winsta0\\Default";' \
        'secure probe launch           pid=%lu desktop-pid=%lu PASS' \
        'SECURE-PROBE-LOG-LIVE' \
        'AERO-PROBE-LIVE' \
        'WaitForSingleObject(child.hProcess, 1000)' \
        'WaitForSingleObject(aeroChild.hProcess, 1000)' \
        'TerminateProcess(child.hProcess, ERROR_TIMEOUT)' \
        'TerminateProcess(aeroChild.hProcess, ERROR_TIMEOUT)' \
        'probeAeroEnableCompositionWhenReady' \
        'AERO DwmEnableComposition initial' \
        'AERO WinSAT(DWM) skipped=composition-enabled PASS' \
        'probeAeroExtendFrameWhenReady' \
        'probeAeroEnableBlurWhenReady' \
        'hr != DWM_E_COMPOSITIONDISABLED' \
        'Triton glass pixel proof' \
        'proof-pattern backdrop='; do
        rg -Fq "$required" \
            "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" || \
            fail "the controlled Aero pixel-proof window is incomplete: $required"
    done
    relay_start_line="$(rg -n -m1 -F \
        'probeRelayFileGrowth(const char *path' "$runtime_probe" | \
        cut -d: -f1)"
    relay_end_line="$(rg -n -m1 -F \
        'probeFileContainsText(const char *path' "$runtime_probe" | \
        cut -d: -f1)"
    if [[ -z "$relay_start_line" || -z "$relay_end_line" ||
          "$relay_start_line" -ge "$relay_end_line" ]] ||
       sed -n "${relay_start_line},${relay_end_line}p" "$runtime_probe" |
           rg -Fq 'probeUploadTo('; then
        fail 'the live public-probe relay can still block on an NTFS flush'
    fi
    aero_initial_line="$(rg -n -m1 -F \
        'probeHr("AERO DwmEnableComposition initial"' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" | \
        cut -d: -f1)"
    aero_winsat_fallback_line="$(rg -n -m1 -F \
        '(void)probeRunWinSatDwm();' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" | \
        cut -d: -f1)"
    if [[ -z "$aero_initial_line" || -z "$aero_winsat_fallback_line" ||
          "$aero_initial_line" -ge "$aero_winsat_fallback_line" ]]; then
        fail 'the Aero probe does not request composition before its WinSAT fallback'
    fi
    secure_live_line="$(rg -n -m1 -F \
        'if (probeRelayFileGrowth(secureLogPath, "SECURE-PROBE-LOG-LIVE"' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" | \
        cut -d: -f1)"
    uxsms_start_line="$(rg -n -m1 -F \
        '(void)probeEnsureUxSmsRunning();' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" | \
        cut -d: -f1)"
    if [[ -z "$secure_live_line" || -z "$uxsms_start_line" ||
          "$secure_live_line" -ge "$uxsms_start_line" ]]; then
        fail 'the public D3D9 transcript is not relayed before DWM startup'
    fi
    python3 "$workspace/scripts/verify_aero_glass_pixels.py" --self-test || \
        fail 'the Aero PNG verifier does not reject the opaque Basic model'
    python3 "$workspace/scripts/capture_vista_passive_png.py" --self-test || \
        fail 'the passive PNG capture gate does not enforce two guest proof runs'

    # Vista's hardware-device admission preserves two pre-D3D9 masks.  Their
    # legacy names are no longer all present in current d3d9caps.h, so retain
    # a source-level contract check instead of regressing to a Basic-only HAL.
    for required in \
        'D3DPMISCCAPS_MASKPLANES' \
        'D3DPMISCCAPS_CONFORMANT' \
        'D3DPMISCCAPS_FOGINFVF' \
        'D3DPRASTERCAPS_DITHER' \
        'D3DPRASTERCAPS_ROP2' \
        'D3DPRASTERCAPS_FOGVERTEX' \
        'D3DPTFILTERCAPS_MIPFPOINT' \
        'D3DPTFILTERCAPS_MIPFLINEAR' \
        'TextureFilterCaps must accept MIL point and linear mip state' \
        'PrimitiveMiscCaps & 0x0000002b == 0x0000002b' \
        'Shader Model 2 requires separate fog in FVF' \
        'RasterCaps        & 0x00000093 == 0x00000093'; do
        rg -Fq "$required" "$ddi" || \
            fail "the Vista UMD Level-1 caps contract is incomplete: $required"
    done
    rg -Fq 'caps checked SM2 fog-in-FVF admission via GetDeviceCaps PASS' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" || \
        fail 'the public D3D9 probe does not pin Vista SM2 fog-in-FVF admission'
    rg -Fq 'testing the filtered caps for 0x2000 produces a false failure' \
        "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c" || \
        fail 'the public D3D9 probe does not account for Vista cap normalization'

    rg -Fq 'CreateDCA("DISPLAY", dev.DeviceName, NULL, NULL)' "$renderer" || \
        fail "the Vista UMD does not open the selected display DC correctly"
    rg -Fq '#define NPT_D3DKMT_CLIENT_HINT D3DKMT_CLIENTHINT_DX9' "$renderer" || \
        fail "the Vista UMD does not use the Vista-compatible DX9 context hint"
    rg -Fq 'virtgpu_query_adapter_info(gpu, &info, sizeof(info))' "$renderer" || \
        fail "the Vista UMD adapter search does not probe the private Triton ABI"
    # The Vista D3D9 runtime path must report the actual callback contract it
    # receives.  A generic E_FAIL here otherwise looks like a login or DWM
    # failure even though it happens before the first Neptune command.
    for required in \
        'npt_d3d9_runtime_callback_mask' \
        'NPT_D3D9_REQUIRED_CALLBACKS' \
        'D3D9 runtime binding interface=0x%x version=0x%x callbacks=0x%03x' \
        'D3D9 runtime callbacks incomplete interface=0x%x version=0x%x have=0x%03x need=0x%03x missing=0x%03x'; do
        rg -Fq "$required" "$renderer" || \
            fail "the Vista UMD cannot identify an incomplete runtime callback table"
    done
    # The compile-only ABI target pins the exact Vista prefix of the device
    # callback table that the transport consumes.  Do not let a later global
    # WDDM header/version change silently shift those callback slots.
    for required in \
        'Vista D3D8-caps query value changed' \
        'Vista D3D8-compatible caps prefix changed' \
        'Vista Allocate callback offset changed' \
        'Vista Render callback offset changed' \
        'Vista Escape callback offset changed' \
        'Vista CreateContext callback offset changed' \
        'Vista device callback table changed'; do
        rg -Fq "$required" "$abi_test" || \
            fail "the Vista D3D9 callback ABI guard is missing: $required"
    done
    for required in \
        'D3DDDICAPS_GETD3D8CAPS' \
        'TRITON9-GETCAPS D3D8' \
        'TRITON9-D3D8CAPS-NEED' \
        'adapter->info.V1.IamVioGPU != VIOGPU_IAM' \
        '!adapter->info.V1.Flags.Supports3d' \
        '!adapter->info.V1.Flags.HasShmem' \
        '(1ull << VIOGPU_CAPSET_NEPTUNE)'; do
        rg -Fq "$required" "$ddi" || \
            fail "the Vista D3D9 adapter pairing check is missing: $required"
    done

    # Standard allocations supply only allocation-private bytes.  The KMD
    # must reject short buffers and unknown formats instead of returning
    # uninitialized resource bytes or silently substituting BGRA.
    for required in \
        'pStandardAllocation->ResourcePrivateDriverDataSize = 0' \
        'return STATUS_BUFFER_TOO_SMALL' \
        '!TryColorFormat(surfaceData->Format, &format)' \
        'const BOOLEAN detachFromHost = m_BackingAttachedToHost' \
        'm_BackingAttachedToHost = attachToHost' \
        'SetDxPhysicalAddress(0)'; do
        rg -Fq "$required" "$allocation" || \
            fail "the Vista KMD allocation contract is missing: $required"
    done

    # Vista advertises aperture residency, so paging must move and initialize
    # real bytes.  A success-only stub silently loses allocation contents.
    rg -Fq 'return allocation->PagingTransfer(pBuildPagingBuffer);' "$paging" || \
        fail "the Vista KMD paging transfer is still a no-op"
    rg -Fq 'return allocation->PagingFill(pBuildPagingBuffer);' "$paging" || \
        fail "the Vista KMD paging fill is still a no-op"
    rg -Fq 'VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID)' "$allocation" || \
        fail "the Vista KMD cannot fill or transfer its fixed primary segment"
    rg -Fq 'GetFrameBufferVA' "$allocation" || \
        fail "the Vista KMD fixed primary paging path cannot access framebuffer bytes"
    rg -Fq 'CopyToFixedPrimary' "$allocation" || \
        fail "the Vista KMD basic present path does not populate the fixed primary"
    rg -Fq 'case VIOGPU_CMD_COPY_FIXED_PRIMARY:' "$workspace/triton-kmd/viogpu/viogpu3d/viogpu_command.cpp" || \
        fail "the Vista KMD does not defer its fixed-primary copy until DMA execution"
    rg -Fq 'copyHeader->type = VIOGPU_CMD_COPY_FIXED_PRIMARY;' "$workspace/triton-kmd/viogpu/viogpu3d/viogpu_device.cpp" || \
        fail "the Vista KMD Present path does not queue its fixed-primary copy"
    rg -Fq 'cmdBody->res_id = dst->GetId();' "$workspace/triton-kmd/viogpu/viogpu3d/viogpu_device.cpp" || \
        fail "the Vista KMD does not upload its fixed primary after the CPU blt"
    rg -Fq 'flushHeader->type = VIOGPU_CMD_FLUSH_FIXED_PRIMARY;' \
        "$workspace/triton-kmd/viogpu/viogpu3d/viogpu_device.cpp" || \
        fail "the Vista KMD does not queue a flush after the fixed-primary upload"
    rg -Fq 'case VIOGPU_CMD_FLUSH_FIXED_PRIMARY:' \
        "$workspace/triton-kmd/viogpu/viogpu3d/viogpu_command.cpp" || \
        fail "the Vista KMD cannot execute its ordered post-transfer flush"
    if rg -Fq 'transfer, content lost' "$paging"; then
        fail "the Vista KMD still contains the content-loss paging path"
    fi
    if sed -n '/VioGpu3DSetPointerPosition/,/VioGpu3DSetPointerShape/p' "$paging" | \
       rg -Fq 'return STATUS_NOT_IMPLEMENTED'; then
        fail "the Vista KMD rejects CDD's required pointer-disable acknowledgement"
    fi
    rg -Fq '|| res->IsPrimary()' "$vidpn" || \
        fail "the Vista KMD treats fixed-primary segment offset zero as no scanout"
    rg -Fq 'pChildStatus->Rotation.Angle = 0;' \
        "$workspace/triton-kmd/viogpu/viogpu3d/viogpu_adapter.cpp" || \
        fail "the Vista KMD does not satisfy the required rotation-status query"

    # Pointer-sized guest values remain 64-bit on the Neptune wire.  On x86,
    # cast-through-pointer serialization reads beyond the source object.
    rg -Fq 'const uint64_t wire = (uint64_t)*val;' "$protocol" || \
        fail "the Neptune SIZE_T encoder does not widen through a value"

    # A reset or removal also wakes the private render event.  The UMD must
    # query execution state before it reports that the command retired.
    for required in \
        'PFND3DKMT_GETDEVICESTATE getDeviceState' \
        'D3DKMT_DEVICESTATE_EXECUTION' \
        'D3DKMT_DEVICEEXECUTION_ACTIVE' \
        'GETPROC(getDeviceState, GetDeviceState)'; do
        rg -Fq "$required" "$renderer" || \
            fail "the Vista UMD completion path is missing: $required"
    done

    # Failed output binds must leave both the D3D9 shadow state and D3D11 host
    # state unchanged.  Otherwise one rejected DWM bind poisons later draws.
    rg -Fq 'triton9RestoreTextureResource' "$output" || \
        fail "the Vista UMD output path lacks texture-state rollback"
    rg -Fq '(void)triton9BindOutputs(device);' "$output" || \
        fail "the Vista UMD output path lacks host output rollback"

    # Vista asks a newly-created software-vertex-processing HAL for the
    # post-transform vertex-cache description.  This is an acceptance
    # handshake, not an optional rendering feature.  Keep the DDI callback
    # and values aligned with the Vista-era WDDM contract: the CACHE FOURCC,
    # longest-strips optimizer, and an unspecified cache size.
    for required in \
        'triton9GetInfo' \
        'D3DDDIDEVINFOID_VCACHE' \
        'funcs->pfnGetInfo = triton9GetInfo;' \
        'cache->Pattern = 0x48434143u;' \
        'cache->OptMethod = 0;' \
        'cache->CacheSize = 0;' \
        'cache->MagicNumber = 0;' \
        'return S_OK;'; do
        rg -Fq "$required" "$unsupported" || \
            fail "the Vista D3D9 VCache acceptance contract is missing: $required"
    done

    # MIL's hardware test installs its canonical state block before it draws
    # anything.  These are legal, inert values for Triton's unlit compositor
    # path; rejecting them makes MIL disable the adapter before the first
    # clear/readback can establish the actual transport path.
    for required in \
        'triton9SetMaterial' \
        'device->material = *args;' \
        'D3DDDIRS_LASTPIXEL' \
        'D3DDDIRS_TWOSIDEDSTENCILMODE' \
        'D3DDDIRS_DIFFUSEMATERIALSOURCE' \
        'D3DDDIRS_SPECULARMATERIALSOURCE' \
        'D3DDDIRS_AMBIENTMATERIALSOURCE' \
        'D3DDDIRS_AMBIENT' \
        'args->Value != args->Stage'; do
        rg -Fq "$required" "$unsupported" "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp" || \
            fail "the Vista MIL default D3D9 state contract is missing: $required"
    done

    # The public runtime seeds the fog parameter payloads even while both fog
    # modes are NONE.  They are DWORD/float payloads, not stencil-op enums;
    # accepting FOGCOLOR=0 is a device-creation requirement.  Keep the
    # acceptance branch physically before the stencil validation so a future
    # case-label merge cannot turn CreateDeviceEx into E_FAIL again.
    fog_parameter_block=$(sed -n \
        '/case D3DDDIRS_FOGCOLOR:/,/case D3DDDIRS_STENCILFAIL:/p' "$state")
    [[ "$fog_parameter_block" == *'return S_OK;'* ]] || \
        fail 'the Vista D3D9 fog parameter defaults are not accepted before stencil validation'

    # Level-1 device validation also sets texture × COLOR1.  Accepting that
    # D3DTA_SPECULAR input without preserving it in the fixed-function pixel
    # shader would make the public state test pass while later composition
    # takes a different path.
    for required in \
        'D3DTA_SPECULAR' \
        'Triton9FixedSource::Specular' \
        'hasSpecular'; do
        rg -Fq "$required" "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp" "$shader" || \
            fail "the Vista MIL COLOR1 fixed-function contract is missing: $required"
    done

    # CreateDeviceEx runs a Level-1 state handshake before the D3D9 device is
    # usable.  State-only callbacks must not instantiate Neptune there: the
    # proxy itself issues runtime Allocate/Render callbacks.  A first resource,
    # non-null shader, or draw is the only point allowed to acquire the proxy.
    for required in \
        'A null binding is only local D3D9 state' \
        'Creation-time constants are D3D9 state' \
        'if (device->hostContext)' \
        'triton9SetStreamSource' \
        'triton9SetIndices'; do
        rg -Fq "$required" "$shader" || \
            fail "the Vista D3D9 creation-state firewall is missing: $required"
    done
    for required in \
        'triton9Flush' \
        'Flush has its ordinary D3D11 meaning' \
        'if (!device->hostContext)'; do
        rg -Fq "$required" "$ddi" || \
            fail "the Vista D3D9 creation-flush firewall is missing: $required"
    done
    # State-block traffic is part of the runtime's creation bookkeeping.  It
    # is local to Triton's D3D9 state shadow and must not be mistaken for an
    # unsupported rendering feature or a reason to acquire the proxy.
    for required in \
        'triton9StateSet' \
        'TRITON9-STATESET-OP' \
        'state-block commands are runtime bookkeeping' \
        'return S_OK;'; do
        rg -Fq "$required" "$unsupported" || \
            fail "the Vista D3D9 creation state-set contract is missing: $required"
    done
    for required in \
        'state-only flight recorder' \
        'TRITON9-RS-REJECT-STATE' \
        'TRITON9-TSS-REJECT-STAGE' \
        'TRITON9-TSS-REJECT-HR'; do
        rg -Fq "$required" "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp" || \
            fail "the Vista D3D9 creation-state flight recorder is missing: $required"
    done

    # Vista composition clients select hardware vertex processing only when
    # the D3D9 HAL advertises this bit.  The guest-owned probe must prove the
    # matching path with untransformed vertices and explicit W/V/P state.
    for required in \
        'D3DDEVCAPS_HWTRANSFORMANDLIGHT' \
        'D3DDEVCAPS_PUREDEVICE' \
        'triton9SetTransform' \
        'triton9UploadFixedVertexConstants'; do
        rg -Fq "$required" "$ddi" "$workspace/triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp" || \
            fail "the Vista D3D9 hardware-vertex contract is missing: $required"
    done

    # Vista implements DrawPrimitiveUP as SetStreamSourceUm followed by the
    # ordinary DrawPrimitive callback.  DrawPrimitive2 is a separate,
    # transformed stream-zero callback whose offsets are measured in bytes.
    # Keep those paths distinct and make the upload/range arithmetic testable.
    for required in \
        'triton9BindUpVertexStreams' \
        'triton9BindDraw2VertexStreamZero' \
        'triton9RestoreTemporaryVertexStreams' \
        'D3D11_USAGE_DEFAULT' \
        'UpdateSubresource' \
        'TRITON9-DRAW-PREP-STAGE'; do
        rg -Fq "$required" "$shader" || \
            fail "the Vista D3D9 user-memory draw path is missing: $required"
    done
    for required in \
        'triton9_draw_um_prefix_bytes' \
        'triton9_draw_indexed_vertex_range' \
        'triton9_draw2_vertex_window' \
        'triton9_draw2_normalize_indices'; do
        rg -Fq "$required" "$draw_contract" "$draw_contract_test" || \
            fail "the Vista D3D9 draw arithmetic contract is missing: $required"
    done
    rg -Fq 'bytes == 48' "$draw_contract_test" || \
        fail "the Vista D3D9 DrawPrimitiveUP triangle byte contract is not tested"
    for required in \
        'triton9_sm2_dcl_semantic' \
        'triton9_sm2_dcl_sampler' \
        'triton9_sm2_replicate_x' \
        '0x80000000u' \
        '0x80010005u' \
        '0x90000000u' \
        '0xa0000000u'; do
        rg -Fq "$required" "$shader_token_contract" "$shader_token_test" || \
            fail "the Vista public SM2 token contract is missing: $required"
    done
    rg -Fq 'triton9_sm2_dcl_semantic(D3DDECLUSAGE_POSITION, 0)' "$runtime_probe" || \
        fail "the Vista public probe does not use runtime-valid SM2 DCL tokens"
    rg -Fq 'probeShaderSourceReplicateX(D3DSPR_CONST, 0)' "$runtime_probe" || \
        fail "the Vista public probe does not encode oPos.w from c0.x"
    for required in \
        'TRITON9-CREATE-VS-SIZE' \
        'TRITON9-CREATE-VS-HR' \
        'TRITON9-CREATE-PS-SIZE' \
        'TRITON9-CREATE-PS-HR'; do
        rg -Fq "$required" "$shader" || \
            fail "the Vista programmable shader flight recorder is missing: $required"
    done

    # The probe is an in-guest service.  It records D3D results on the Vista
    # disk; it must never require a host web server or wait on host networking
    # at every D3D boundary.
    for required in \
        'TRITON9-PROBE PASS' \
        'TRITON9-RUN nonce=%s' \
        'probeGenerateResultNonce' \
        'caps HW transform+light      PASS' \
        'CreateDeviceEx HWP' \
        'D3DCREATE_MULTITHREADED' \
        'D3DCREATE_HARDWARE_VERTEXPROCESSING' \
        'D3DCREATE_PUREDEVICE' \
        'MIL default state contract' \
        'MIL Level1 state contract' \
        'D3DRS_TWOSIDEDSTENCILMODE' \
        'D3DTA_SPECULAR' \
        'DrawPrimitiveUP COLOR1 triangle' \
        'COLOR1 triangle center=0x%08lx expected=0xff00ff00 %s' \
        'D3DTSS_TEXCOORDINDEX' \
        'IDirect3DDevice9Ex_SetMaterial' \
        'IDirect3DDevice9Ex_CheckDeviceState' \
        'IDirect3DDevice9Ex_WaitForVBlank' \
        'CreateAdditionalSwapChain' \
        'AdditionalSwapChain Present' \
        'CreateVertexShader vs_2_0' \
        'CreatePixelShader ps_2_0' \
        'DrawPrimitiveUP SM2 triangle' \
        'SM2 triangle center=0x%08lx %s' \
        'TRITON9_VISTA_MANAGED_POOL' \
        'Create Vista-managed lockable shader texture' \
        'Lock/upload shader texture' \
        'CreateVertexShader textured SM2' \
        'CreatePixelShader textured SM2' \
        'D3DSTT_2D' \
        'D3DSIO_TEX' \
        'SetPixelShaderConstantF' \
        'DrawPrimitiveUP textured SM2 triangle' \
        'textured SM2 center=0x%08lx expected=0x%08lx %s' \
        'IDirect3DDevice9Ex_CreateOffscreenPlainSurface' \
        'Create SYSTEMMEM bitmap update surface' \
        'Create DEFAULT bitmap sample texture' \
        'UpdateSurface bitmap texture' \
        'CheckResourceResidency bitmap texture' \
        'DrawPrimitiveUP bitmap UpdateSurface triangle' \
        'bitmap UpdateSurface center=0x%08lx expected=0x%08lx %s' \
        'D3DFVF_XYZ | D3DFVF_DIFFUSE' \
        'D3DTS_PROJECTION' \
        'clear pixel=0x%08lx expected=0xff112233 %s' \
        'triangle center=0x%08lx %s' \
        'TRITON9-AERO API-PATH PASS VISUAL-UNVERIFIED' \
        'DwmIsCompositionEnabled' \
        'DwmEnableComposition' \
        'DwmExtendFrameIntoClientArea' \
        'DwmEnableBlurBehindWindow' \
        'CheckFormat D24S8 depth/stencil' \
        'Create D24S8 depth/stencil' \
        'Clear D24S8 depth' \
        'WinSAT(DWM)' \
        'winsta0\\Default' \
        'FlushFileBuffers(g_log)'; do
        rg -Fq "$required" "$runtime_probe" || \
            fail "the self-contained Vista D3D9 probe is missing: $required"
    done
    if rg -q '10\.0\.2\.2|wininet\.dll|Internet(Open|Connect|Send)' "$runtime_probe"; then
        fail "the Vista D3D9 probe still depends on host-network transport"
    fi

    # The deployment service no longer invokes SetupAPI against the active
    # display stack.  Vista can hold that path indefinitely.  It verifies
    # immutable optical media, enters Safe Mode, stages every payload beside
    # its destination, removes optical-media read-only attributes from old
    # destinations, activates and verifies the unloaded graphics files, and
    # delays only replacement of its own live service executable.
    for required in \
        'stage_payload_replacement' \
        'activate_staged_payload' \
        'schedule_staged_payload' \
        'MoveFileExW' \
        'MOVEFILE_WRITE_THROUGH' \
        'MOVEFILE_DELAY_UNTIL_REBOOT' \
        'INSTALL_PAYLOAD_DESTINATION_WRITABLE' \
        'attributes & ~FILE_ATTRIBUTE_READONLY' \
        'FlushFileBuffers(pending_file)' \
        'set_safe_mode' \
        'SAFE_INSTALL_BLOCKED' \
        'commit_safe_activation_return_state' \
        'DRIVE_CDROM' \
        'AMBIGUOUS_MEDIA' \
        'SAFEBOOT_MINIMAL' \
        'SAFEBOOT_NETWORK' \
        'SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Option' \
        'is_safe_boot' \
        'RegFlushKey' \
        'SetLastError(ERROR_SUCCESS)' \
        'AdjustTokenPrivileges' \
        'InitiateSystemShutdownExW' \
        'probe_sha256' \
        'install_pinned_signing_certificate' \
        'SIGNING_CERT_SHA256' \
        '2464DC7241B33AF0E6D333ED6D7542ADAD59DC1C' \
        'CERT_SYSTEM_STORE_LOCAL_MACHINE' \
        'verify_authenticode_file' \
        'verify_catalog_member' \
        'TRITON_VISTA_BYPASS_CUSTOM_TRUST 1' \
        'PACKAGE_SIGNATURES_BYPASSED id=%ls integrity=manifest-sha256' \
        'disable_boot_integrity_enforcement' \
        '/set {current} testsigning on' \
        '/set {current} nointegritychecks on' \
        '/set {current} loadoptions DDISABLE_INTEGRITY_CHECKS' \
        'BOOT_INTEGRITY_BYPASS_OK testsigning=on' \
        'FAILED_BOOT_INTEGRITY_BYPASS' \
        'WINTRUST_CATALOG_INFO' \
        'CryptCATAdminCalcHashFromFileHandle' \
        'verify_deployment_media' \
        'verify_installed_payloads' \
        'PROBE_EXE_NAME' \
        'ProbeResultPath' \
        'ProbeResultNonce' \
        'QueryServiceStatus' \
        'format_probe_result_log' \
        'TRITON9-RUN nonce=' \
        'VerifiedSuccessId' \
        'PostVerifyRetry' \
        'REG_OPTION_VOLATILE' \
        'ACTIVATION_BOOT_GUARD_KEY' \
        'REPROBE_BOOT_GUARD_KEY' \
        'ActivationGuardId' \
        'ReprobeGuardId' \
        'DeleteFileW(result_log)' \
        'probe_log_has_completed_success' \
        'TRITON9-PROBE PASS' \
        'TRITON9-AERO API-PATH PASS VISUAL-UNVERIFIED' \
        'ReprobeState' \
        'REPROBE_REBOOT' \
        'REPROBE_LAUNCHED' \
        'POST_REBOOT_COMMIT'; do
        rg -Fq "$required" "$deploy_service" || \
            fail "the guest-owned Vista deploy service is missing: $required"
    done
    rg -Fq 'FILE_SHARE_READ | FILE_SHARE_WRITE' "$deploy_service" || \
        fail 'the guest-owned deploy service cannot share COM2 telemetry with the public probe'
    probe_stage_block=$(sed -n \
        '/static BOOL stage_optional_probe/,/^}/p' "$deploy_service")
    [[ "$probe_stage_block" == *'SERVICE_DEMAND_START'* ]] || \
        fail 'the public probe is not demand-started after installed-byte verification'
    [[ "$probe_stage_block" != *'SERVICE_AUTO_START'* ]] || \
        fail 'the public probe can race deployment verification as an auto-start service'
    driver_install_block=$(sed -n \
        '/static BOOL install_driver/,/^}/p' "$deploy_service")
    [[ $(print -r -- "$driver_install_block" | \
          rg -c -F 'schedule_staged_payload(') == 1 &&
       "$driver_install_block" == *'needs_replacement[3]'* &&
       "$driver_install_block" == *'for (index = 0; index < 3; ++index)'* ]] || \
        fail 'the guest-owned deploy service can schedule a mixed graphics generation'
    normal_install_block=$(sed -n \
        '/static BOOL install_normal_mode/,/^}/p' "$deploy_service")
    [[ "$normal_install_block" != *'install_driver('* &&
       "$normal_install_block" == *'disable_boot_integrity_enforcement()'* &&
       "$normal_install_block" == *'set_safe_mode()'* &&
       "$normal_install_block" == *'disable_boot_integrity_enforcement()'*'set_safe_mode()'* ]] || \
        fail 'normal mode can replace an active Triton graphics payload'
    if rg -Fq 'SetupCopyOEMInfW' "$deploy_service" || \
       rg -Fq 'UpdateDriverForPlugAndPlayDevicesW' "$deploy_service"; then
        fail "the guest-owned Vista deploy service still uses active-stack SetupAPI"
    fi
    if rg -Fq 'GetSystemMetrics' "$deploy_service"; then
        fail "the guest-owned Vista deploy service still enters USER32 during recovery"
    fi
    embedded_signature_block=$(sed -n \
        '/static const WCHAR \*const embedded_files\[\]/,/^    };/p' \
        "$deploy_service")
    [[ "$embedded_signature_block" == *'L"viogpu3d.sys"'* ]] || \
        fail 'the guest-owned deploy service does not verify the copied miniport embedded signature'
    [[ $(rg -c -F 'write_state(L"LastSuccessId"' "$deploy_service") == 1 ]] || \
        fail 'LastSuccessId can be written before the post-reboot byte check'

    for required in \
        'readonly=on' \
        'vista-deploy-current' \
        'vista-deploy-runs' \
        'media.iso.sha256' \
        'run-token' \
        'deployment-id' \
        'qemu-pid' \
        'qemu-path' \
        'run_dir="$run_parent/r-${run_token[1,12]}"' \
        'qmp_socket_bytes=' \
        '"$qmp_socket_bytes" -lt 104' \
        'media_iso="$bundle_canonical/media.iso"' \
        'diagnostic_parent=$(realpath' \
        'lsof -t -- "$vista_disk"' \
        '-serial "file:$status_log"'; do
        rg -Fq -- "$required" "$guest_launcher" || \
            fail "the guest-owned Vista launcher is missing: $required"
    done
    if rg -qi 'sendkey|human-monitor-command|input-send-event|qmpinput|vmkey|osascript|AXUIElement|Proscenium|-monitor[[:space:]]' \
        "$guest_launcher"; then
        fail 'the guest-owned Vista launcher contains a host-input or GUI-control path'
    fi
    if rg -qi -- '-serial[[:space:]].*tcp:|tcp:0\.0\.0\.0' "$guest_launcher"; then
        fail 'the guest-owned Vista launcher exposes a bidirectional serial input path'
    fi
    [[ $(rg -c -F 'execute(stream, "qmp_capabilities")' "$passive_capture") == 1 ]] || \
        fail 'the passive capture helper does not negotiate QMP exactly once'
    [[ $(rg -c -F '"screendump"' "$passive_capture") == 1 ]] || \
        fail 'the passive capture helper must contain exactly one PNG screendump command'
    for required in \
        'validate_run_directory' \
        'deployment run identity does not match' \
        'deployment QEMU process does not own the run endpoints' \
        '/usr/sbin/lsof' \
        'os.link(' \
        'follow_symlinks=False'; do
        rg -Fq "$required" "$passive_capture" || \
            fail "the passive capture run binding is missing: $required"
    done
    if rg -qi 'sendkey|human-monitor-command|input-send-event|qmpinput|vmkey|ppm' \
        "$passive_capture"; then
        fail 'the passive capture helper contains forbidden input or raw-frame behavior'
    fi
    for required in \
        'df -Pk "$transfer_root"' \
        'maximum_media_bytes=$((512 * 1024 * 1024))' \
        'vista-deploy-publications' \
        'media.iso.sha256' \
        'deployment bundle pointer must be a safe leaf' \
        'mv -fh -- "$pointer_stage" "$bundle_link"' \
        'ISO9660/Joliet'; do
        rg -Fq -- "$required" "$media_stager" || \
            fail "the bounded read-only media stager is missing: $required"
    done
}

if (( audit_only )); then
    log "checking the Vista KMD source contract"
    python3 "$workspace/triton-kmd/viogpu/tools/check_vista_kmd_source.py"
    audit_source_build_contract
    log "source and build contract audit completed"
    exit 0
fi

if (( build_package )); then
    build_available_kib=$(df -Pk "$workspace" | awk 'NR == 2 { print $4 }')
    [[ "$build_available_kib" == <-> ]] || \
        fail 'cannot determine free space before the package build'
    (( build_available_kib >= 8 * 1024 * 1024 )) || \
        fail 'less than 8 GiB is free; refusing to build the Vista package'
    log "package-build free space: $build_available_kib KiB"
    if ! prlctl list -a --json | rg -q '"name": "Tiny11"'; then
        fail "the Tiny11 Parallels build VM is unavailable"
    fi

    log "checking the Vista KMD source contract"
    python3 "$workspace/triton-kmd/viogpu/tools/check_vista_kmd_source.py"
    audit_source_build_contract

    build_vista_umds
    build_vista_deploy_service
    generate_source_manifest "$source_manifest"

    if (( ! repackage_only )); then
        log "building Vista x64 and x86 KMDs from the synchronized source tree in Windows 11"
        run_windows_batch 'Y:\test-artifacts\windows11_build_vista_serialtrace_kmd.bat'

        log "signing the KMD"
        run_windows_batch 'Y:\test-artifacts\windows11_sign_vista_serialtrace_kmd.bat'
    else
        mkdir -p "$transfer_root"
        if [[ ! -s "$transfer_root/viogpu3d-vista-x64.sys" ]]; then
            cp -f "$workspace/test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d.sys" \
                "$transfer_root/viogpu3d-vista-x64.sys"
        fi
        if [[ ! -s "$transfer_root/viogpu3d-vista-x86.sys" ]]; then
            cp -f "$workspace/test-artifacts/vista-driver-x86/viogpu3d.sys" \
                "$transfer_root/viogpu3d-vista-x86.sys"
        fi
        for artifact in "$transfer_root/viogpu3d-vista-x64.sys" \
                        "$transfer_root/viogpu3d-vista-x86.sys"; do
            test -s "$artifact" || fail "cannot restage without signed KMD: $artifact"
        done
        log "reusing the current signed KMDs for UMD/service-only package restage"
    fi

    log "building and signing Vista x64 and x86 packages ($driver_date $driver_version)"
    run_windows_batch 'Y:\test-artifacts\windows11_stage_vista_pnp_package.bat' \
        "$package_name_x64" "$package_name_x86" "$driver_date" "$driver_version"

    post_build_manifest=$(mktemp /tmp/vista-unified-source-post.XXXXXX)
    generate_source_manifest "$post_build_manifest"
    if ! cmp -s "$source_manifest" "$post_build_manifest"; then
        rm -f "$post_build_manifest"
        fail "reviewed source changed while the package was being built"
    fi
    rm -f "$post_build_manifest"
fi

if (( probe_only )); then
    # Probe-only runs intentionally keep the manifest-verified driver package
    # unchanged, but the executable placed on the Safe Mode ISO must always
    # come from the current probe source rather than an older transfer copy.
    build_vista_probes
fi

package_dir="$transfer_root/$package_name_x64"
x86_package_dir="$transfer_root/$package_name_x86"
for artifact in \
    viogpu3d.sys \
    viogpu3d-diagnostic.inf \
    viogpu3d-vista-x64.cat \
    neptune_d3d9.dll \
    neptune_d3d9_wow.dll \
    triton-vista-deploy.exe \
    triton9_runtime_probe_x64.exe; do
    test -s "$package_dir/$artifact" || fail "missing staged artifact: $package_dir/$artifact"
done
for artifact in \
    viogpu3d.sys \
    viogpu3d.inf \
    viogpu3d-vista-x86.cat \
    neptune_d3d9.dll; do
    test -s "$x86_package_dir/$artifact" || \
        fail "missing staged x86 artifact: $x86_package_dir/$artifact"
done

audit_package x64 "$package_dir"
audit_package x86 "$x86_package_dir"

if (( build_package )); then
    rg -Fq "DriverVer = $driver_date,$driver_version" "$package_dir/viogpu3d-diagnostic.inf" || \
        fail "the staged INF does not contain the generated version"
    rg -Fq "DriverVer = $driver_date,$driver_version" "$x86_package_dir/viogpu3d.inf" || \
        fail "the staged x86 INF does not contain the generated version"

    cp -f "$source_manifest" "$package_dir/source-manifest.sha256"
    cp -f "$source_manifest" "$x86_package_dir/source-manifest.sha256"
    (
        cd "$package_dir"
        shasum -a 256 \
            viogpu3d.sys \
            viogpu3d-diagnostic.inf \
            viogpu3d-vista-x64.cat \
            neptune_d3d9.dll \
            neptune_d3d9_wow.dll \
            triton-vista-deploy.exe \
            triton9_runtime_probe_x64.exe \
            source-manifest.sha256 > package-manifest.sha256
    )
    (
        cd "$x86_package_dir"
        shasum -a 256 \
            viogpu3d.sys \
            viogpu3d.inf \
            viogpu3d-vista-x86.cat \
            neptune_d3d9.dll \
            source-manifest.sha256 > package-manifest.sha256
    )
elif test -s "$package_dir/package-manifest.sha256"; then
    log "verifying the existing package manifest"
    (cd "$package_dir" && shasum -a 256 -c package-manifest.sha256)
    test -s "$x86_package_dir/package-manifest.sha256" || \
        fail "the deploy-only x86 package has no package-manifest.sha256"
    (cd "$x86_package_dir" && shasum -a 256 -c package-manifest.sha256)
else
    fail "the deploy-only package has no package-manifest.sha256"
fi

if (( ! deploy_package )); then
    log "package build completed"
    shasum -a 1 \
        "$package_dir/viogpu3d.sys" \
        "$package_dir/viogpu3d-vista-x64.cat" \
        "$x86_package_dir/viogpu3d.sys" \
        "$x86_package_dir/viogpu3d-vista-x86.cat"
    exit 0
fi

# From this point onward, any failure must leave the qcow2 stopped.  Success
# disarms cleanup so the validated accelerated desktop remains available for
# the next D3D9Ex/DWM test gate.
vista_cleanup_armed=1

if (( ! runtime_only )); then
cp -f "$workspace/test-artifacts/vista-install-current-driver.cmd" \
    "$transfer_root/vista-install-current-driver.cmd"
# cmd.exe can execute the leading LF-only lines but its CALL :label scanner on
# Vista does not reliably find later labels without DOS line endings. Normalize
# the served copy so :download is always discoverable in the guest.
perl -pi -e 's/\r?\n\z/\r\n/' \
    "$transfer_root/vista-install-current-driver.cmd"
python3 - "$transfer_root/vista-install-current-driver.cmd" <<'PY'
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
if b'\n' in data.replace(b'\r\n', b''):
    raise SystemExit('Vista installer contains a bare LF')
PY

# Mount the package as read-only media during Safe Mode.  The currently
# installed display miniport wedges the normal recovery session before its
# shell can download a replacement.  Safe Mode enumerates the PCI device for
# devcon but does not start the display miniport, so it is the reliable place
# to replace the KMD and both D3D9 UMDs.
build_vista_deploy_iso

# Restart the exact named server once per run so endpoint changes and newly
# staged packages are never hidden by an old long-lived Python process.
screen -S vista_build_artifacts -X quit >/dev/null 2>&1 || true
terminate_exact_artifact_server
rm -f "$probe_result" "$service_result"
log "starting the Vista artifact server"
screen -dmS vista_build_artifacts \
    python3 "$workspace/scripts/vista_artifact_server.py" \
    --port "$artifact_port" --root "$transfer_root"
for attempt in {1..20}; do
    curl -fsSI "http://127.0.0.1:$artifact_port/$package_name_x64/viogpu3d.sys" >/dev/null && break
    sleep 0.25
done

curl -fsSI "http://127.0.0.1:$artifact_port/$package_name_x64/viogpu3d.sys" >/dev/null || \
    fail "the staged driver is not available from the artifact server"
curl -fsSI "http://127.0.0.1:$artifact_port/vista-install-current-driver.cmd" >/dev/null || \
    fail "the guest installer is not available from the artifact server"

if (( probe_only )); then
    # Replacing only the standalone probe and registering its one-shot
    # service does not require display-driver replacement.  Use the normal
    # Standard VGA recovery desktop with the same read-only ISO; this avoids
    # making probe iteration depend on bootmgr's timing-sensitive F8 menu.
    # Detach KD here: it cannot add evidence during Standard VGA staging and
    # can hold the recovery boot at a checked-kernel debugger handshake.  KD
    # is armed again immediately before the accelerated Triton boot below.
    log "detaching checked KD for the Standard VGA probe-staging boot"
    stop_checked_kd
    vista_checked_kd_active=0
    start_vista_vm recovery
    wait_for_vista_recovery_desktop
else
    # The checked image can resume the previous one-shot probe's user-mode
    # heap assertion before Safe Mode reaches the package updater. Safe Mode
    # uses Standard VGA and does not start the display miniport, so keep KD
    # detached for this recovery-only phase. The final accelerated boot below
    # always attaches checked KD before Triton can load.
    log "detaching checked KD for the Standard VGA Safe Mode package update"
    stop_checked_kd
    vista_checked_kd_active=0
    ensure_vista_safe_mode
fi
open_vista_command_prompt
if (( ! probe_only )); then
    rm -f "$install_marker"
    log "installing the current package from the read-only Safe Mode deployment ISO"
    python3 "$workspace/scripts/qmpinput.py" type \
        'cls & for %D in (D E F G H I) do @if exist %D:\vista-install-current-driver.cmd call %D:\vista-install-current-driver.cmd vista-driver-x64-pnp-current %D:'
    python3 "$workspace/scripts/qmpinput.py" key ret
    for attempt in {1..300}; do
        install_text=$(vista_screen_text || true)
        guard_vista_runtime 'waiting for verified Vista driver installation' "$install_text"
        if [[ "${install_text:u}" == *'OKINSTALLREADYOK'* ]]; then
            log "Vista verified the driver installation from read-only media"
            break
        fi
        sleep 1
    done
    [[ "${install_text:u}" == *'OKINSTALLREADYOK'* ]] || \
        fail "Vista did not verify the Safe Mode driver installation"
else
    log "reusing the already verified Triton package; staging only the secure-desktop probe"
fi
log "staging the D3D9Ex bring-up probe for the next accelerated boot"
python3 "$workspace/scripts/qmpinput.py" type \
    'for %D in (D E F G H I) do @if exist %D:\vista-stage-d3d9-probe.cmd call %D:\vista-stage-d3d9-probe.cmd'
python3 "$workspace/scripts/qmpinput.py" key ret
wait_for_vista_screen_marker PROBEREADY 60 || \
    fail "Vista did not stage the D3D9Ex boot probe"
if (( probe_only )); then
    # A previous package-install run can leave safeboot in BCD.  Probe staging
    # accepts that recovery mode, but the final Triton-only boot must not.
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /deletevalue {current} safeboot >nul 2>nul'
    python3 "$workspace/scripts/qmpinput.py" key ret
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /deletevalue {default} safeboot >nul 2>nul'
    python3 "$workspace/scripts/qmpinput.py" key ret
    python3 "$workspace/scripts/qmpinput.py" type 'cls'
    python3 "$workspace/scripts/qmpinput.py" key ret
    python3 "$workspace/scripts/qmpinput.py" type \
        'bcdedit /enum all|findstr /i safeboot>nul'
    python3 "$workspace/scripts/qmpinput.py" key ret
    python3 "$workspace/scripts/qmpinput.py" type \
        'if errorlevel 1 echo NORMALREADY'
    python3 "$workspace/scripts/qmpinput.py" key ret
    wait_for_vista_screen_marker NORMALREADY 60 || \
        fail "Vista still has a persistent safeboot entry after probe staging"
fi
stop_vista_vm
fi

# Standard VGA exists only during installation/recovery.  Arm KD before the
# clean accelerated boot, which has Triton as its only display device.
launch_checked_kd
start_vista_vm accelerated

debug_log=''
debug_offset=0
if test -f "$debug_pointer"; then
    debug_log=$(<"$debug_pointer")
fi

# A healthy running Vista kernel does not answer KD's RESET handshake merely
# because a debugger connects.  It will synchronize automatically if a later
# checked diagnostic occurs.  Requiring an artificial initial stop here would
# stall every successful boot and perturb the display timing we are testing.

log "waiting for DriverEntry, hardware start, an active VidPn, stable scanout, and the D3D9 probe after reboot"
saw_driver_entry=0
saw_hardware_start=0
saw_active_commit=0
saw_primary_scanout=0
visible_scanout_streak=0
probe_passed=0
for attempt in {1..900}; do
    if (( attempt % 2 == 1 )); then
        screen_text=$(vista_screen_text || true)
        guard_vista_runtime 'waiting for the accelerated display gates' "$screen_text"
        if (( attempt % 6 == 1 )); then
            guard_checked_kd_bugcheck 'waiting for the accelerated display gates'
        fi
    fi
    if test -n "$debug_log" && test -f "$debug_log"; then
        new_trace=$(tail -c +$((debug_offset + 1)) "$debug_log" | tr -d '\r')
        if (( ! saw_driver_entry )) && [[ "$new_trace" == *'VIOGPU FULL build'* ]]; then
            saw_driver_entry=1
            log "gate 1 passed: the new KMD entered DriverEntry"
        fi
        if (( ! saw_hardware_start )) && \
           [[ "$new_trace" == *'VISTA-START: hardware initialization and capset discovery complete'* ]]; then
            saw_hardware_start=1
            log "gate 2 passed: StartDevice completed hardware initialization"
        fi
        if (( ! saw_active_commit )) && \
           [[ "$new_trace" == *'VISTA-VIDPN: CommitVidPn completed status=0x0 active=1'* ]]; then
            saw_active_commit=1
            log "gate 3 passed: Vista committed an active display mode"
        fi
        if (( ! saw_primary_scanout )) && \
           [[ "$new_trace" == *'scanout 3d res_id='* ]]; then
            screen_text=$(vista_screen_text || true)
            guard_vista_runtime 'validating the accelerated primary scanout' "$screen_text"
            if [[ "$screen_text" != *'Display output is not active'* ]] && \
               vista_screen_has_visible_pixels; then
                (( visible_scanout_streak += 1 ))
                if (( visible_scanout_streak >= 10 )); then
                    saw_primary_scanout=1
                    log "gate 4 passed: the Vista primary remained non-black across ten QMP frames"
                fi
            else
                visible_scanout_streak=0
            fi
        fi
    fi

    # The display gates prove only that dxgkrnl selected a primary.  They do
    # not prove that Vista imported the D3D9 UMD or that the frame contains a
    # usable desktop.  The one-shot service uploads the probe incrementally;
    # accept only its explicit terminal marker from this run.
    if test -s "$probe_result"; then
        probe_text=$(<"$probe_result")
        if [[ "$probe_text" == *'TRITON9-PROBE FAIL'* ]]; then
            log "the fresh Vista D3D9 probe failed:"
            print_d3d9_failure_summary "$probe_text"
            fail "Vista reached the D3D9 runtime but did not complete bring-up"
        elif [[ "$probe_text" == *'TRITON9-PROBE PASS'* ]]; then
            probe_passed=1
        fi
    fi
    if test -s "$service_result"; then
        service_text=$(<"$service_result")
        if [[ "$service_text" == *'TRITON9-PROBE FAIL'* ]]; then
            log "the fresh Vista D3D9 probe failed:"
            print_d3d9_failure_summary "$service_text"
            fail "Vista reached the D3D9 runtime but did not complete bring-up"
        elif printf '%s\n' "$service_text" | \
           rg -q '^secure probe (launch|wait).*[[:space:]]FAIL\r?$'; then
            log "the fresh Vista probe service failed:"
            print_d3d9_failure_summary "$service_text"
            fail "Vista could not execute the D3D9 probe"
        fi
    fi

    if (( saw_driver_entry && saw_hardware_start && saw_active_commit &&
          saw_primary_scanout && probe_passed )); then
            log "deployment reached stable scanout and passed the D3D9 bring-up probe"
            printf '%s\n' "$probe_text"
            shasum -a 1 "$package_dir/viogpu3d.sys" "$package_dir/viogpu3d-vista-x64.cat"
            vista_cleanup_armed=0
            exit 0
    fi
    sleep 1
done

if test -S "$qmp_socket"; then
    printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"/tmp/vista-deploy-timeout.png","format":"png"}}\n' | \
        nc -w 5 -U "$qmp_socket" >/dev/null || true
fi
fail "Vista deployment stopped at gates DriverEntry=$saw_driver_entry StartDevice=$saw_hardware_start ActiveCommit=$saw_active_commit StablePrimaryScanout=$saw_primary_scanout D3D9Probe=$probe_passed; inspect C:\\pnpserial7\\deploy.log, $debug_log, and /tmp/vista-deploy-timeout.png"

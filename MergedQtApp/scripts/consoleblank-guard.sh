#!/bin/sh

CONSOLEBLANK="/sys/module/kernel/parameters/consoleblank"
TTY="/dev/tty1"

fix_consoleblank()
{
    echo "[consoleblank-guard] disable console blanking"

    if [ -w "$CONSOLEBLANK" ]; then
        echo 0 > "$CONSOLEBLANK" 2>/dev/null
    fi

    if [ -e "$TTY" ]; then
        TERM=linux /usr/bin/setterm \
            --blank=0 \
            --powersave=off \
            --powerdown=0 \
            < "$TTY" > "$TTY" 2>/dev/null
    fi
}

echo "[consoleblank-guard] started"

fix_consoleblank

# systemd 的 ExecStartPre 使用 once，确保首次禁用息屏完成后才放行 Weston；
# 默认或 run 模式继续常驻监测，防止运行期间参数被其他组件改回。
case "${1:-run}" in
    once) exit 0 ;;
    run) ;;
    *)
        echo "Usage: $0 [once|run]" >&2
        exit 2
        ;;
esac

while true; do
    if [ -r "$CONSOLEBLANK" ]; then
        VALUE=$(cat "$CONSOLEBLANK" 2>/dev/null)

        if [ "$VALUE" != "0" ]; then
            echo "[consoleblank-guard] consoleblank changed to $VALUE, restoring to 0"
            fix_consoleblank
        fi
    fi

    sleep 2
done


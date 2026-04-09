#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Uso:
  ethernet_quick_test.sh set-ip <interfaz> <ip_cidr>
  ethernet_quick_test.sh ping <ip_destino>
  ethernet_quick_test.sh run-server <puerto>
  ethernet_quick_test.sh run-client <usuario> <ip_servidor> <puerto>

Ejemplos:
  ./scripts/ethernet_quick_test.sh set-ip enp3s0 192.168.50.10/24
  ./scripts/ethernet_quick_test.sh ping 192.168.50.11
  ./scripts/ethernet_quick_test.sh run-server 8080
  ./scripts/ethernet_quick_test.sh run-client alice 192.168.50.10 8080
EOF
}

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

cmd="$1"
shift

case "$cmd" in
  set-ip)
    if [[ $# -ne 2 ]]; then usage; exit 1; fi
    iface="$1"
    ip_cidr="$2"
    sudo ip addr flush dev "$iface"
    sudo ip addr add "$ip_cidr" dev "$iface"
    sudo ip link set "$iface" up
    ip -br addr show dev "$iface"
    ;;
  ping)
    if [[ $# -ne 1 ]]; then usage; exit 1; fi
    ping -c 3 "$1"
    ;;
  run-server)
    if [[ $# -ne 1 ]]; then usage; exit 1; fi
    exec ./bin/chat_server "$1"
    ;;
  run-client)
    if [[ $# -ne 3 ]]; then usage; exit 1; fi
    exec ./bin/chat_client "$1" "$2" "$3"
    ;;
  *)
    usage
    exit 1
    ;;
esac

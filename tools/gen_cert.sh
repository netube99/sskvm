#!/usr/bin/env bash
# 给 P4 签一张「浏览器信任」的证书，消掉自签警告。
#
# 原理：本地建一个 CA（~/.sskvm-ca/），客户端信任这个 CA 一次，
#       之后 P4 的证书（SAN 里带 IP，没有名称不匹配）就完全合法。
#
#   ./tools/gen_cert.sh 192.168.1.50         # 生成/覆盖 main/certs/
#   然后 idf.py build 重新把证书打进固件
#
# 客户端装 CA：
#   Linux : sudo cp ~/.sskvm-ca/ca.crt /etc/ca-certificates/trust-source/anchors/ && sudo update-ca-trust
#   macOS : sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain ~/.sskvm-ca/ca.crt
#   Windows: 双击 ca.crt → 安装到「受信任的根证书颁发机构」
#
# 注意：WebCodecs 需要 secure context，所以局域网里也别退回纯 HTTP。
set -euo pipefail

IP="${1:?usage: gen_cert.sh <p4-ip> [dns-name]}"
DNS="${2:-sskvm.local}"
CA="${HOME}/.sskvm-ca"
OUT="$(cd "$(dirname "$0")/.." && pwd)/main/certs"
mkdir -p "$CA" "$OUT"

# 1) 一次性建 CA
if [ ! -f "$CA/ca.key" ] || [ ! -f "$CA/ca.crt" ]; then
    echo "== 生成本地 CA（$CA）"
    openssl ecparam -name prime256v1 -genkey -noout -out "$CA/ca.key"
    openssl req -x509 -new -key "$CA/ca.key" -out "$CA/ca.crt" -days 3650 \
        -subj "/CN=sskvm local CA" -addext "basicConstraints=critical,CA:TRUE"
fi

# 2) 设备密钥 + CSR
openssl ecparam -name prime256v1 -genkey -noout -out "$OUT/prvtkey.pem"
openssl req -new -key "$OUT/prvtkey.pem" -out "$CA/device.csr" -subj "/CN=sskvm"

# 3) 用 CA 签，SAN 带上 IP 和主机名
printf 'subjectAltName=IP:%s,DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' \
    "$IP" "$DNS" > "$CA/device.ext"
openssl x509 -req -in "$CA/device.csr" -CA "$CA/ca.crt" -CAkey "$CA/ca.key" -CAcreateserial \
    -out "$OUT/servercert.pem" -days 3650 -extfile "$CA/device.ext" 2>/dev/null

echo "== 证书已生成："
openssl x509 -in "$OUT/servercert.pem" -noout -subject -ext subjectAltName | sed 's/^/   /'
echo
echo "下一步："
echo "  1. 把 $CA/ca.crt 装进客户端信任列表（见本脚本头部注释）"
echo "  2. idf.py build && idf.py flash"

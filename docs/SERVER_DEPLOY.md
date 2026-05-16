# Server Deploy

This guide is for running the bot on an Ubuntu VPS. Start with `dry_run`; switch
to `live` only after the server has run stably.

## 1. Install Dependencies

```bash
sudo apt update
sudo apt install -y git cmake build-essential pkg-config \
  libboost-all-dev libssl-dev libsecp256k1-dev
```

## 2. Build

```bash
git clone <repo-url> polymarket
cd polymarket
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j
```

## 3. Configure Dry Run

```bash
cp config/config.example.json config/config.json
mkdir -p logs
```

Keep this for the first server run:

```json
"strategy": {
  "mode": "dry_run"
}
```

If the server needs a proxy to reach Polymarket/Binance/Yahoo, set:

```json
"network": {
  "proxy_url": "http://127.0.0.1:7897",
  "api_host": "127.0.0.1",
  "api_port": 5819
}
```

## 4. Run Manually First

```bash
./build/polymarket-arb config/config.json
```

Use an SSH tunnel for the dashboard before setting up public access.

```bash
ssh -L 5819:127.0.0.1:5819 root@SERVER_IP
```

Open:

```text
http://127.0.0.1:5819
```

## 5. Public Dashboard With IP + Caddy Basic Auth

The bot should keep listening on localhost only:

```json
"network": {
  "api_host": "127.0.0.1",
  "api_port": 5819
}
```

Expose it through Caddy on public port 5820 with a password. This avoids
conflicting with another project that already uses port 80:

```bash
sudo apt install -y debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
  | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
  | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo apt update
sudo apt install -y caddy
```

Create a password hash:

```bash
caddy hash-password --plaintext 'choose-a-dashboard-password'
```

Edit `/etc/caddy/Caddyfile`:

```caddyfile
:5820 {
    basicauth {
        admin <paste-caddy-password-hash-here>
    }

    reverse_proxy 127.0.0.1:5819
}
```

If UFW is enabled, allow the public dashboard port:

```bash
sudo ufw allow 5820/tcp
```

Reload Caddy:

```bash
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl reload caddy
```

Open:

```text
http://SERVER_IP:5820
```

This is HTTP because there is no domain for automatic TLS. Do not reuse an
important password.

## 6. Run With systemd

Edit `deploy/polymarket.service.example` if the server user/path is not
`ubuntu` and `/home/ubuntu/polymarket`. For a root-only server, use:

```ini
User=root
WorkingDirectory=/root/polymarket
ExecStart=/root/polymarket/build/polymarket-arb /root/polymarket/config/config.json
```

```bash
sudo cp deploy/polymarket.service.example /etc/systemd/system/polymarket.service
sudo systemctl daemon-reload
sudo systemctl enable polymarket
sudo systemctl start polymarket
sudo journalctl -u polymarket -f
```

## 7. Live Mode Notes

For live mode, upload `keystore.enc`, fill `wallet.address`,
`polymarket.proxy_address`, and `polymarket.api_address`, then create:

```bash
sudo install -m 600 /dev/null /etc/polymarket.env
sudo nano /etc/polymarket.env
```

Contents:

```text
KEYSTORE_PASSWORD=your-password
```

Then uncomment `EnvironmentFile=/etc/polymarket.env` in the systemd service.

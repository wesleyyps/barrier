# Using Barrier with VPNs (NordLayer, NordVPN, WireGuard, OpenVPN, Tailscale)

When running Barrier alongside a Virtual Private Network (VPN) or enterprise Zero Trust / SASE client (such as **NordLayer**, **NordVPN**, **Cisco AnyConnect**, **Zscaler**, **OpenVPN**, **WireGuard**, or **Tailscale**), local KVM traffic can be disrupted:
- Loss of connectivity between the Barrier client and server (`Connection refused`, `Timed out`, or silent packet drops).
- Local network isolation where secondary physical interfaces (like dedicated Ethernet cables) become completely blocked.
- Locked corporate VPN profiles where **Split Tunneling cannot be enabled** due to enterprise security policies (MDM/Zero Trust).

This guide explains the underlying network architecture, how Barrier natively handles dynamic multi-homed failover between **Cable (Primary)** and **Wi-Fi (Secondary)**, and the steps to configure your environment for seamless operation.

---

## Table of Contents
1. [Root Causes: Why VPNs Break Local KVM Traffic](#root-causes-why-vpns-break-local-kvm-traffic)
2. [The Architectural Solution: Dual-Network Failover (Cable + Wi-Fi)](#the-architectural-solution-dual-network-failover-cable--wi-fi)
   - [Why Cable Normally?](#why-cable-normally)
   - [Why Wi-Fi with VPN?](#why-wi-fi-with-vpn)
   - [How Barrier Solves This Natively (Dynamic Discovery, Cache & Auto-Failback)](#how-barrier-solves-this-natively)
3. [Client Cache & Operational Flow](#client-cache--operational-flow)
4. [VPN Client Configuration (When Unlocked)](#vpn-client-configuration-when-unlocked)
   - [NordLayer](#nordlayer)
   - [NordVPN](#nordvpn)
   - [OpenVPN](#openvpn)
   - [WireGuard](#wireguard)
   - [Tailscale](#tailscale)
5. [Firewall & Port Requirements](#firewall--port-requirements)
6. [Troubleshooting & Diagnostics](#troubleshooting--diagnostics)

---

## Root Causes: Why VPNs Break Local KVM Traffic

### 1. Default Gateway Hijacking & Kernel Packet Filtering
Standard VPNs reroute outbound traffic through a virtual tunnel adapter (`utun` on macOS, `tun0` on Linux). More aggressively, enterprise SASE clients (such as NordLayer) install OS-level network filter extensions (`NEFilterDataProvider` on macOS, WFP callouts on Windows) that enforce a strict **Kill Switch**. 
- Even if you add static OS routes (`sudo route add ...`), the VPN kernel filter intercepts and drops packets leaving via non-VPN interfaces.

### 2. Multi-Homed Subnet Blind Spots (The Cable vs Wi-Fi Problem)
Most high-performance setups use two network connections simultaneously:
- **Wi-Fi** (e.g. `192.168.15.0/24`): Connected to the Internet router via DHCP.
- **Dedicated Ethernet Cable / Switch** (e.g. `192.168.1.0/24`): Fixed IPs with ultra-low latency (<1ms) dedicated to Barrier.

When a VPN enables "Local Network Access" (LAN access), it almost always **only whitelists the primary default interface (Wi-Fi)**. Secondary physical adapters (such as the USB-C Ethernet adapter for the cable) are treated as unauthorized networks and completely blocked by the VPN filter.

### 3. Corporate Policy Locks (No Split Tunneling)
In corporate deployments, the **Split Tunneling** toggle in NordLayer or Cisco AnyConnect is frequently locked by the organization's administrator. Users cannot manually bypass subnets or apps.

---

## The Architectural Solution: Dual-Network Failover (Cable + Wi-Fi)

To achieve the best of both worlds — **microsecond latency on cable** during normal operation, and **continuous operation when VPN is active** — Barrier implements native **Dynamic Multi-Homed Failover**.

```
                           +-------------------------------------+
                           |         Barrier Server (.101)       |
                           |  Cable: 192.168.1.101 (Fixed)       |
                           |  Wi-Fi: 192.168.15.10 (DHCP dynamic)|
                           +------------------+------------------+
                                              |
                     +------------------------+------------------------+
                     | SADD Announcement:                              |
                     | "192.168.1.101,192.168.15.10"                   |
                     v                                                 v
   +------------------------------------+            +------------------------------------+
   |       Barrier Client (.102)        |            |        Barrier Client (.103 Mac)   |
   | Normal: Connects to 192.168.1.101  |            | Normal: Connects to 192.168.1.101  |
   | VPN ON: Fails over to 192.168.15.10|            | VPN ON: Fails over to 192.168.15.10|
   | Auto-Probe: Returns to Cable (.101)|            | Auto-Probe: Returns to Cable (.101)|
   +------------------------------------+            +------------------------------------+
```

### Why Cable Normally?
Physical Ethernet cable provides dedicated bandwidth, no wireless interference, zero packet jitter, and sub-millisecond cursor latency across machines.

### Why Wi-Fi with VPN?
When NordLayer or corporate VPN connects, it allows communication on the local Wi-Fi router's subnet (`192.168.15.0/24`) while dropping the secondary cable subnet (`192.168.1.0/24`). Wi-Fi therefore acts as the resilient fallback path.

### How Barrier Solves This Natively

1. **Dynamic Multi-Homed IP Advertising (`kMsgDServerAddresses` / `SADD`)**:
   - The server is configured with the primary fixed cable IP (e.g. `192.168.1.101:24800`).
   - Upon client connection, the server inspects its active physical interfaces (`Server::discoverLocalAddresses()`), automatically excluding virtual bridges, containers (`docker*`, `br-*`, `veth*`), and VPN tunnels (`utun*`, `tun*`, `tap*`).
   - The server advertises its list of physical addresses to all connected clients via the `SADD` protocol message.

2. **Persistent Local Cache (`server_fallback.cache`)**:
   - Because Wi-Fi uses dynamic DHCP, the server's Wi-Fi IP can change over time.
   - Whenever the client is connected over cable, it receives the latest Wi-Fi IP and writes it to a persistent local cache:
     - **macOS**: `~/Library/Application Support/barrier/server_fallback.cache`
     - **Linux**: `~/.local/share/barrier/server_fallback.cache`
     - **Windows**: `%LOCALAPPDATA%\Barrier\server_fallback.cache`
   - If the client is restarted while the VPN is already active (when cable is blocked), the client automatically reads the fallback IP from this cache and connects over Wi-Fi without manual intervention!

3. **Sub-Second Failover (< 1s)**:
   - If the primary cable drops or is blocked by the VPN, the client immediately cycles to the secondary Wi-Fi address within ~500ms.

4. **Transparent Auto-Failback with Background Probe**:
   - While operating over the fallback Wi-Fi connection, the client launches a periodic non-blocking background TCP probe (every 10 seconds) targeting port 24800 of the primary cable IP.
   - As soon as the VPN is disconnected (or the cable is restored), the probe succeeds and the client automatically switches back to the primary low-latency cable connection.

---

## Client Cache & Operational Flow

| Scenario | Primary Interface (Cable) | Secondary Interface (Wi-Fi) | Client Action |
| :--- | :--- | :--- | :--- |
| **Normal (VPN Off)** | Reachable (`192.168.1.101`) | Reachable (`192.168.15.10`) | Connects via **Cable**. Caches server's current Wi-Fi IP. |
| **VPN Connected (NordLayer)** | **Blocked by VPN Filter** | Reachable (`192.168.15.10`) | Fails over to **Wi-Fi** using cached IP in < 1s. Background probe runs every 10s. |
| **VPN Disconnected** | Reachable again | Reachable | Background probe succeeds; client **fails back to Cable** automatically. |
| **Client Boot with VPN Active** | **Blocked** | Reachable | Reads `server_fallback.cache`, connects directly via **Wi-Fi**. |

---

## VPN Client Configuration (When Unlocked)

If your VPN client allows changing settings (not locked by an organization MDM profile), verify the following:

### NordLayer

1. **Enable Local Network Access**:
   - Open **NordLayer** > **Preferences / Settings** (`Cmd + ,` on macOS).
   - Go to **Connection** (or **General**).
   - Ensure **Allow Local Network Access** (or **LAN Access**) is toggled **ON**.
   - Ensure **Invisibility on LAN** is toggled **OFF**.

2. **Split Tunneling (if unlocked)**:
   - Go to **Settings** > **Split Tunneling**.
   - If application-based split tunneling is enabled, add **Barrier** (`barrierc` / `barriers`).
   - If subnet-based split tunneling is enabled, add both your Cable subnet (`192.168.1.0/24`) and Wi-Fi subnet (`192.168.15.0/24`).

3. **Threat Prevention / Web Protection**:
   - If long-lived connections (IDE AI streams, language servers, Barrier sockets) get reset upon connecting, disable or relax **Threat Prevention** in NordLayer.

### NordVPN

1. Open NordVPN **Settings**.
2. Under **General / Connection**:
   - Toggle **Stay invisible on LAN** to **OFF**.
   - Toggle **Allow LAN traffic** to **ON**.
3. Under **Split Tunneling**:
   - Enable **Split Tunneling** and set Barrier (`barrierc`, `barriers`) to **Bypass VPN**.

### OpenVPN

In `.ovpn` client profiles, tell OpenVPN to route local subnets via the physical gateway rather than the VPN tunnel:
```ini
route 192.168.1.0 255.255.255.0 net_gateway
route 192.168.15.0 255.255.255.0 net_gateway
```

### WireGuard

In `wg0.conf`, ensure `AllowedIPs` does not specify `0.0.0.0/0`. Instead, specify only remote subnets, or exclude `192.168.1.0/24` and `192.168.15.0/24`.

### Tailscale

Preserve local LAN access while routing through an exit node:
```bash
tailscale up --exit-node=<exit-node-ip> --exit-node-allow-lan-access=true
```

---

## Firewall & Port Requirements

Barrier communicates over TCP port **24800** by default.

1. **Barrier Server**: Must accept inbound TCP on port 24800 on all physical interfaces (Cable and Wi-Fi):
   - **Linux (`ufw`)**: `sudo ufw allow 24800/tcp`
   - **Linux (`iptables`)**: `sudo iptables -A INPUT -p tcp --dport 24800 -j ACCEPT`
   - **macOS**: Allow `Barrier.app` and `barriers` in **System Settings** > **Network** > **Firewall**.
   - **Windows**: Add an Inbound Rule in Windows Defender Firewall for TCP port 24800.

2. **Barrier Client**: Must allow outbound TCP to port 24800 on both subnets (`192.168.1.x` and `192.168.15.x`).

Test connectivity from any client:
```bash
# Test primary cable
nc -zv 192.168.1.101 24800

# Test secondary Wi-Fi
nc -zv 192.168.15.10 24800
```

---

## Troubleshooting & Diagnostics

### 1. Check Server Advertised Addresses
In the server log (`/home/wsousa/barrier.log` on Linux or via GUI log):
```text
NOTE: advertising server physical addresses to client "wesley-Inspiron-3583": 192.168.1.101,192.168.15.10
```

### 2. Inspect Client Fallback Cache
- **macOS**:
  ```bash
  cat "$HOME/Library/Application Support/barrier/server_fallback.cache"
  # Output: 192.168.1.101,192.168.15.10
  ```
- **Linux**:
  ```bash
  cat "$HOME/.local/share/barrier/server_fallback.cache"
  # Output: 192.168.1.101,192.168.15.10
  ```

### 3. Check for Stale or Self-Referencing ARP Entries
If a static ARP entry was incorrectly set on macOS or Linux, traffic may loop back to localhost:
```bash
# macOS
arp -a -n
# If server IP is listed as 'permanent' with your own MAC, delete it:
sudo arp -d 192.168.1.101

# Linux
sudo ip neigh flush dev eth0
```

### 4. Verify Active Routes
Check which interface resolves the connection:
```bash
# macOS / Linux
route get 192.168.1.101   # Should resolve to physical Ethernet (e.g. en6)
route get 192.168.15.10   # Should resolve to Wi-Fi (e.g. en0)
```

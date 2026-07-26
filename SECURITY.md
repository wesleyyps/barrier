# Security and Privacy

Barrier is designed to bridge your keyboard and mouse across multiple devices on your network. Because it transmits your keystrokes, clipboard contents, and mouse movements, securing this data is absolutely critical.

## Strict Encryption (TLS)

By default, Barrier relies heavily on TLS (Transport Layer Security) encryption to safeguard your data. 

**As of the latest security hardening update:**
- Legacy TLS versions (TLS 1.0 and TLS 1.1) have been explicitly disabled to prevent protocol downgrade attacks (such as POODLE or BEAST).
- The OpenSSL cipher suite has been strictly locked down to modern, high-security algorithms, ensuring Perfect Forward Secrecy.
- **Plaintext Connections Are No Longer Supported:** Previously, users could launch Barrier with the `--disable-crypto` flag to transmit keystrokes in plaintext over the network. Because Barrier relies on a "Trust On First Use" (TOFU) certificate fingerprint for authentication (rather than a password), allowing plaintext connections meant that *anyone on your local network could instantly hijack your server without a password*. 
- The `--disable-crypto` flag is now ignored, and all connections are strictly encrypted and authenticated.

---

## Alternative: SSH Tunneling

If you are operating in an environment where you absolutely cannot use Barrier's built-in TLS (e.g., connecting a very old legacy client that doesn't support modern TLS, or you simply prefer managing access via SSH keys), you can tunnel Barrier's traffic through an SSH tunnel.

Because Barrier now strictly enforces crypto on its default listeners, you must tunnel the connection properly:

### 1. Establish the SSH Tunnel
Run the following command from the **Client** machine to establish a secure tunnel to the **Server**. (Assuming the Server is at `192.168.1.100` and the Barrier port is `24800`):

```bash
ssh -N -L 24800:127.0.0.1:24800 user@192.168.1.100
```
*This binds your client's local port `24800` directly to the server's local port `24800` through an encrypted SSH tunnel.*

### 2. Configure Barrier Client
Because the SSH tunnel is listening on your local machine, tell your Barrier Client to connect to `localhost`:

```bash
barrierc localhost
```

**Note:** If you are using SSH tunneling, your connection is authenticated by your SSH keys and encrypted by the SSH daemon. You still cannot use `--disable-crypto`, but because you are connecting over `localhost`, the traffic stays securely within the tunnel.

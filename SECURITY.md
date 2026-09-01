# Security policy

## Supported versions

Security fixes are applied to the current master branch and included in future releases.

## Reporting a vulnerability

Use [GitHub private vulnerability reporting](https://github.com/wrick17/denon/security/advisories/new).
Do not open a public issue for a suspected vulnerability.

Include the affected firmware or integration version, the impact, and concise
reproduction steps. Redact Wi-Fi credentials, Home Assistant tokens, device
addresses, entity IDs, and network addresses from reports and screenshots.

Keep the ESP32 web interface on a trusted local network. Do not expose it to
the internet through port forwarding or a public reverse proxy. Complete the
guided Wi-Fi, receiver, and Home Assistant setup while physically present.
After setup, the service is designed to run unattended on the local network.

# How to Get Remote Access to the Anycubic M5s
The printer exposes the default telnet and SSH ports, either on its own network or on yours if you
have it connected.

# Tangent: A Brief History of the M5s for Absolutely No Reason
The anycubic Photon Mono M5s began development in 2022 and was released to the public in 2023 with a
sticker price of $399. This printer was super appealing, as it was low-priced and sported a 12K screen
with automatic bed leveling. The M5s was marketed alongside Anycubic's "Speed" resin, which had a lower
viscosity and faster cure time, allowing for much faster print times than their then-current
flagship models. However, the Speed resin was ultra brittle once cured, and the lower viscosity made even
little spills more of a mess, so I personally just used the ABS-like resin and settled for slower
prints that didn't break. A Pro version of the M5s was then introduced in 2024. I dunno shit about
it... what were we talking about? Oh right, remote access...

# Getting the Password
I tried two separate firmware versions: 1.0.2.7 and 1.0.4.5.
Version 1.0.2.7 is the latest version available on their website, and 1.0.4.5 can be installed OTA directly
from the Anycubic app.

For firmware version 1.0.2.7, the password is trivial to crack with brute force. According to
Reddit (the most trustworthy of sources), this vulnerability was reported and the company may have
taken action, since the password for 1.0.4.5 is not the same. A copy of 1.0.2.7 is still available
on their site and in this repo for convenience.

Once you have the password, copying your SSH key to the device can be accomplished with the
following command:

```
cat ~/.ssh/id_rsa.pub | ssh anycubic 'mkdir -p /etc/dropbear && cat > /etc/dropbear/authorized_keys && chmod 600 /etc/dropbear/authorized_keys'
```

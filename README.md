# lechat

Simple peer-to-peer libevent chat.

How to build:

```
sudo apt-get install libevent-dev cmake3
mkdir build
cd build
cmake ../
make
```

How to run:

```
chmod +x lechat
./lechat eth0
```

Understands two operators:
1. message >>> ip_address 
    Connect to a specified address, if not connected, and send a message.
2. name or ip_address === name 
    Rename a connected recipient.

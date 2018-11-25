# pktfwdbr

This is intended to be a minimal bridge between the semtech lora packet forwarder running on a gateway
and some other system you have that is operating the extra lorawan backend stuff.
The main idea is to get the messages from out of the udp packets and onto MQTT where you can deal with
them with as little fuss as possible.

## mqtt topics

### RX packets 

A little bit of processing is done to received packets to funnel them into sub-topics that might
be useful for a LoRaWAN backend.

```pktfwdbr/<gwid>/rx/[join|unconfirmed|confirmed|other]```

### TX packets

For now there is only one topic for tx packets.

```pktfwdbr/<gwid>/tx```

## message format

The format of the data in the messages are the same as the packet forwarder.
# pktfwdbr

[![Build Status](https://travis-ci.com/fifteenhex/pktfwdbr.svg?branch=master)](https://travis-ci.com/fifteenhex/pktfwdbr)

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

Publishes with the json format documented in the semtech protocol documentation can be sent to
the tx topic. An optional token can be passed in the topic. This token will come back in the txack
so you can correlate which tx the txack is for.

Topic to schedule a tx:

```pktfwdbr/<gwid>/tx/<token>```

Topic where the forward txack will be published:

```pktfwdbr/<gwid>/txack/<token>```

## message format

The format of the data in the messages are the same as the packet forwarder.

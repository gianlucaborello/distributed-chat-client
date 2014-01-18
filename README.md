distributed-chat-client
=======================

Minimal demo chat client made using Protobuf, Redis, ØMQ

BUILD
=====

Make sure the following libraries are installed in the standard system paths (on Debian Sid all these packages are available in the standard repositories):

- hiredis
- protocolbuffer
- zeromq

To build, type "make" from the root directory of the project, the binary "client" will be created.
Run it without arguments to get some help.

DETAILS
=======

The main architecure is really simple and inefficient.
It's basically a full meshed network where each client is connected to every other client.
The pattern used from 0mq is the publisher/subscriber one.

Improvement to this architecture would include:
- Partial meshed network, where each client is connected only to a subset of clients, 
  and the intermediate nodes will take care of the relay.
- Broker architecture: in some way, one client is elected as the master and will take
  care of distributing all the messages.

Huge improvements to the implementation:
- Separate the process in a receiver and sender process, connected via a IPC 0mq socket,
  so that we have two different windows and we don't mess up the layout of the screen, or
  use a ncurses frontend.
- Implement the history: when a client connects to a group, the database of the previous messages
  is sent to him.
- Better error checking: each method returning an exception/error code value should be
  carefully parsed and reported to the user.
- The database is just a dump of the output window. According to the different purposes, fields
  like a timestamp could be added.
- It would be nice to have a close_connection() method in 0q for the socket. Unfortunately there's
  no such feature, the only thing you can do is close the entire socket, so each node will try
  to connect to the clients that left basically forever, using the exponential backoff algorithm.
  Maybe this is a sign that my solution is not pointing to the right direction.

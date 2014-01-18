#include <iostream>
#include <set>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <hiredis.h>
#include <zmq.hpp>

#include "messages.pb.h"

using namespace std;

static int s_interrupted = 0;
static const string TRANSPORT_TYPE = "tcp://";

static void s_signal_handler(int signal_value)
{
	s_interrupted = 1;
}

static void s_catch_signals(void)
{
	struct sigaction action;
	action.sa_handler = s_signal_handler;
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
}

static void s_recv(zmq::socket_t* socket, Message* msg) {
	zmq::message_t message;
	socket->recv(&message, 0);
	int size = message.size();
	char *s = (char*) malloc(size + 1);
	memcpy(s, message.data(), size);
	s[size] = 0;
	string sstr(s, size);
	free(s);
	msg->ParseFromString(sstr);
}

static void s_send(zmq::socket_t* socket, Message* msg)
{
	string smsg;
	msg->SerializeToString(&smsg);
	zmq::message_t message(smsg.size());
	memcpy (message.data(), smsg.c_str(), smsg.size());
	socket->send(message);
}

int main(int argc, char** argv)
{
	if(argc < 3 || argc > 4)
	{
		cerr << endl;
		cerr << "Usage: " << string(argv[0]) << " db_addr:port local_addr:port [remote_group:port]" << endl;
		cerr << "- db_addr:port = IP address/port of the REDIS database used for logging" << endl;
		cerr << "- local_addr:port = IP address/port of a local interface that will be used as identifier for the client" << endl;
		cerr << "- remote_group:port = IP address/port of a remote client. If omitted, a new group will be created" << endl;
		cerr << endl;
		return 1;
	}

	s_catch_signals();

	// This keeps the list of the other clients
	set<string> peers; 
	string my_id(argv[2]);

	zmq::context_t context(1);

	zmq::socket_t receiver(context, ZMQ_SUB);
	
	try
	{
		receiver.bind((TRANSPORT_TYPE + my_id).c_str());
	}
	catch(exception& e)
	{
		cerr << "Invalid local address/port" << endl;
		return 1;
	}
	
	peers.insert(my_id);
	receiver.setsockopt(ZMQ_SUBSCRIBE, "", 0);
	
	zmq::socket_t sender(context, ZMQ_PUB);
	
	// Allow 10 seconds for the exponential backoff algorithm
	// to avoid flooding the network with SYN packets
	int reconnect_time_ms = 10000;
	sender.setsockopt(ZMQ_RECONNECT_IVL_MAX, &reconnect_time_ms, sizeof(reconnect_time_ms));

	if(argc > 3)
	{
		//
		// Connect to a remote group
		//
		string remote_group(argv[3]);
		
		peers.insert(remote_group);

		try
		{
			sender.connect((TRANSPORT_TYPE + remote_group).c_str());
		}
		catch(exception& e)
		{
			cerr << "Invalid remote address/port" << endl;
			return 1;
		}
		
		Message msg;
		msg.set_type(Message::JOIN);
		msg.set_src(my_id);
		s_send(&sender, &msg);
	}
	
	//
	// Poll stdin and the subscriber socket
	//
	zmq::pollitem_t items[] =
	{
		{ receiver, 0, ZMQ_POLLIN, 0 },
		{ NULL, 0, ZMQ_POLLIN, 0 }
	};
	
	string db(argv[1]);
	size_t colon_pos = db.find(":");
	if(colon_pos == string::npos)
	{
		cerr << "Invalid database address" << endl;
		return 1;
	}
	
	redisContext* redis_context = redisConnect(
		db.substr(0, colon_pos).c_str(), 
		atoi(db.substr(colon_pos + 1, db.size() - colon_pos - 1).c_str()));
	
	if(redis_context->err)
	{
		cerr << "Error connecting to the database" << endl;
		return 1;
	}
	
	cout << "Enter your message(s), or press CTRL+C to quit" << endl;
	
	while(true)
	{
		if(s_interrupted) 
		{
			cout << "Leaving the group" << endl;

			Message msg;
			msg.set_type(Message::LEAVE);
			msg.set_src(my_id);
			s_send(&sender, &msg);
			break;
		}

		// Poll every 50ms to be responsive to the CTRL+C request
		if(zmq::poll(items, 2, 50) == 0)
		{
			continue;
		}
		
		if(items[0].revents & ZMQ_POLLIN)
		{
			Message received_msg;
			s_recv(&receiver, &received_msg);
					
			switch(received_msg.type())
			{
				case Message::JOIN:
				{
					//
					// if a new client joined, connect to it and broadcast the list of connected nodes that we currently have
					//
					cout << "Host '" << received_msg.src() << "' joined the group" << endl;

					pair<set<string>::iterator, bool> p = peers.insert(received_msg.src());

					//
					// Add the connection to the socket only if it's the first time we see this client
					//
					if(p.second)
					{
						sender.connect((TRANSPORT_TYPE + received_msg.src()).c_str());
					}
					
					for(set<string>::iterator it = peers.begin(); it != peers.end(); ++it)
					{
						Message msg;
						
						msg.set_type(Message::HOST);
						msg.set_src(my_id);
						msg.set_data(*it);
						s_send(&sender, &msg);
					}

					break;
				}
				case Message::LEAVE:
				{
					cout << "Host '" << received_msg.src() << "' left the group" << endl;
					peers.erase(received_msg.src());
					break;
				}
				case Message::HOST:
				{
					//
					// We are receiving the host list, connect to all the hosts that are not in our peer list
					//
					if(peers.find(received_msg.data()) == peers.end())
					{
						cout << "Host '" << received_msg.data() << "' joined the group" << endl;
						peers.insert(received_msg.data());
						sender.connect((TRANSPORT_TYPE + received_msg.data()).c_str());
					}

					break;
				}
				case Message::MSG:
				{
					string message = "[" + received_msg.src() + "] " + received_msg.data();
					cout << message << endl;
					
					void* reply = redisCommand(redis_context, "RPUSH %s %s", my_id.c_str(), message.c_str());
					if(reply == NULL)
					{
						cerr << "Error logging to the database" << endl;
						return 1;
					}
					freeReplyObject(reply);
					
					break;
				}
				default:
					assert(false);
			}
		}
		
		if(items[1].revents & ZMQ_POLLIN)
		{
			string s;
			getline(cin, s);

			Message msg;
			msg.set_type(Message::MSG);
			msg.set_src(my_id);
			msg.set_data(s);
			s_send(&sender, &msg);

			string message = "[" + my_id + "] " + s;
			cout << message << endl;
			
			void* reply = redisCommand(redis_context, "RPUSH %s %s", my_id.c_str(), message.c_str());
			if(reply == NULL)
			{
				cerr << "Error logging to the database" << endl;
				return 1;
			}
			freeReplyObject(reply);
		}
	}
	
	redisFree(redis_context);
		
	return 0;
}

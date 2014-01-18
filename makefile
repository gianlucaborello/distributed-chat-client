client: client.cpp messages.pb.cc
	g++ -lzmq -lprotobuf -lhiredis -g -Wall -o client client.cpp messages.pb.cc
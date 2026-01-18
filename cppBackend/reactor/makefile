all:client echoserver


client:client.cpp
	g++ -g -o client client.cpp
echoserver:tcpepoll.cpp InetAddress.cpp Socket.cpp Epoll.cpp Channel.cpp Eventloop.cpp tcpserver.cpp Acceptor.cpp Connection.cpp Buffer.cpp EchoServer.cpp ThreadPool.cpp Timestamp.cpp
	g++ -g -o echoserver tcpepoll.cpp InetAddress.cpp Socket.cpp Epoll.cpp Channel.cpp Eventloop.cpp tcpserver.cpp Acceptor.cpp Connection.cpp Buffer.cpp EchoServer.cpp ThreadPool.cpp Timestamp.cpp -lpthread

clean:
	rm -f client echoserver

梳理15中服务器的大致流程:
  1.创建tcpserver类 tcpserver类会初始化Acceptor Eventloop成员变量,并将acceptor的回调函数成员绑定自身的newconnecion函数
    a.因为是loop是栈对象,所以在构造函数前就会初始化
      i.eventloop的初始化会new一个Epoll对象
      ii.Epoll的构造函数会调用epoll_create创建epoll句柄
    b.acceptor的构造函数中构建了listenfd,完成了bind,listen,通过channel将listenfd绑定到epoll上,设置Channel的回调函数,监听listenfd的读文件
  2.main函数直接调用tcpserver.start(),start又会调用EventLoop类的run函数,开始loop函数
    a.loop封装了epoll_wait(),根据epoll_wait监听的fd,封装为channel,获取fd,events(监听的事件),revents(本次监听发生的事件)
      并以channel*数组的形式返回
    b.返回后调用channel的处理函数,他会根据不同类型进行不同的判断,其中如果发生读事件则调用回调函数成员
      i.回调函数会根据不同channel的对象而不同,如果是监听listenfd的channel对象(被封装成了acceptor类),则会调用创建新的客户端连接的函数
      ii.如果是已连接的客户端的channel(被封装成了connection类),则会调用处理函数
    c.服务器第一次loop返回的channel*数组只会有建立listenfd的读事件发生,因此只会调用已有的acceptor类成员
      调用其回调函数,创建一个新的客户端连接
      i.acceptor有两个回调函数,一个是channel类中的回调函数,一个是类自带的回调函数
        loop返回的channel*数组会调用channel类的回调函数,这个回调函数是为了建立新客户端的连接
        此回调函数中,因为要建立connectfd,就要建立Connection类,但是Connection和Acceptor类都是在Channel类的基础上构建的平等的类
        所以不能让Acceptor类创建Connection类,故而使用类自带的回调函数,让tcpserver类完成构造(因为connection对象应该在tcpserver中被构造)
    d.第一次之后的loop则会可能返回不同性质channel(监听fd,客户端fd),随后根据fd不同来调用channel的回调函数   
    

    
    
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include "sock.h"
#include "per_id.h"
#include "a_peer.h"
#include "udp_xdea.h"


extern int __perport;
extern std::string __srvip;
extern bool __provider;

a_peer::a_peer(const char* id):_id(id)
{
    _status = SRV_UNREGISTERED;
    _regtime = 0;
    _pingtime = 0;
    _type = _id[0]=='p' ? 1 : 0;
    system("rm -f /tmp/_p_*");
    _udpbuffer = new uint8_t[MAX_UDP+1];
}

a_peer::~a_peer()
{
    ::close(_fdout);
    delete []_udpbuffer;
}


bool kbhit()
{
    termios term;
    tcgetattr(0, &term);

    termios term2 = term;
    term2.c_lflag &= ~ICANON;
    tcsetattr(0, TCSANOW, &term2);

    int byteswaiting;
    ioctl(0, FIONREAD, &byteswaiting);

    tcsetattr(0, TCSANOW, &term);

    return byteswaiting > 0;
}

void a_peer::main()
{
    udp_xdea    s(__key);
    time_t      now = time(0);
    time_t      iah = 0;
    std::string message=":";

    _pingtime = now;
    _srvsin.from_string(__srvip.c_str(), SRV_PORT);
    strcpy(_mecap._u.reg.meiot,__meikey.c_str());
    strcpy(_mecap._u.reg.id,_id.c_str());
    _mecap._u.reg.typ = _type;

    for(int i=__perport; i< __perport+100;i++)
    {
        if(s.create(i) && s.bind()!=-1)
        {
            __perport=i;
            break;
        }
        s.destroy();
    }

    if(s.socket()>0)
    {
        std::cout << "on port:" << __perport << "\n";
        while(__alive)
        {
            now = time(0);

            if(_status==SRV_UNREGISTERED)
            {
                if(now - _regtime > REG_TICK)
                {
                    _regtime = now;
                    _mecap._verb = SRV_REGISTER;
                    _mecap._u.reg.typ = _type;

                    if(0==s.send((const uint8_t*)&_mecap,sizeof(_mecap),_srvsin,__key))
                    {
                        std::cout << "no server \r\n";
                        break;
                    }
                }
            }
            _receive(s,now);
            if(now - iah > IAM_HERE_TO)
            {
                iah = now;
                _i_am_here(s);
                _pingtime=now;//
                if(now - _pingtime > SERVER_DOWN_TO)
                {
                    std::cout << " ?????    server missing \r\n";
                    _pers.clear();
                    _status=SRV_UNREGISTERED;
                    _pingtime = now;
                }
            }
            if(_status == PER_LINKED)// && __provider)
            {
                if(kbhit())
                {
                    char cha[2]={0};
                    cha[0] = (char)::getchar();
                    if(cha[0] != '\r' && cha[0] != '\n')
                    {
                        message.append(cha);
                    }
                    else {
                        send_to_pers(s, message.c_str(), message.length(),false);
                        message=":";
                    }
                }
            }
        }
    }
}

int a_peer::_rec_udp(udp_xdea& s, time_t now)
{
    int bytes = s.udp_sock::receive((char*)_udpbuffer, MAX_UDP);
    std::cout << ">" << Ip2str(s.Rsin()) << "\n";
    if(bytes>=int(sizeof(SrvCap)))
    {
        return _received(s,now, bytes);
    }
    else if(bytes>0)
    {
        for(const auto& a:_pers)
        {
            if(a==s.Rsin())
            {
                _data_in(s, bytes);
            }
        }
    }
    return 0;
}

bool a_peer::_receive(udp_xdea& s, time_t now)
{
    ipp         newper;
    fd_set      rdSet;
    int         nfds = (int)s.socket()+1;
    timeval     tv   = {0,1024};

    FD_ZERO(&rdSet);
    FD_SET(s.socket(), &rdSet);
    int sel = ::select(nfds, &rdSet, 0, 0, &tv);
    if(sel < 0)
    {
        std::cout << "select fatal \r\n";
        exit(errno);
    }
    if(sel > 0 && FD_ISSET(s.socket(), &rdSet))
    {
        _rec_udp(s,now);
    }
    _proc_perrs(s);
    return true;
}

bool a_peer::_received(udp_xdea& s, time_t now, int bytes)
{
    if(s.Rsin() == _srvsin)
    {
        SrvCap in;
        int    off = 0;
        while(bytes>0)
        {
            ed(_udpbuffer+off, (uint8_t*)&in, sizeof(SrvCap), __key, false);
            std::cout << "got " << bytes <<", "<< " bytes \n";
            _srv_process(s, in, now);
            off+=sizeof(SrvCap);
            bytes -= sizeof(SrvCap);
        }
        assert(bytes==0);
    }
    else
    {
        if(!_per_process(s, bytes, now))
        {
            //deny
        }
    }
    return true;
}

bool a_peer::_srv_process(udp_xdea& s, SrvCap&  plin, time_t now)
{
    switch(plin._verb)
    {
    case SRV_REGISTERRED:
        std::cout << "SRV REGISTERRED " << "\n";
        if(_status == SRV_UNREGISTER)
            _status = SRV_REGISTERRED;
        _regtime = time(0);

        break;

    case SRV_SET_PEER:
        std::cout << "SRV SENT PEERING DATA \n";
        _peering(s, plin);
        break;

    case SRV_UNREGISTERED:
        std::cout << "SRV RELEASED " << "\n";
        _status = SRV_UNREGISTERED;
        break;

    case SRV_PING:
        std::cout << "GOT SRV PING" << IP2STR(s.Rsin()) << "\n";
        if(_status == SRV_UNREGISTER)
            _status = SRV_REGISTERRED;
        break;

    default:
        std::cout << "invalid srv verb " << int(plin._verb) << " from " << Ip2str(s.Rsin()) <<"\r\n";
        std::cout <<  (const char*)plin._u.reg.meiot <<"\r\n";
        break;
    }
    _pingtime = now;
    return true;
}

bool a_peer::_per_process(udp_xdea& s, int bytes, time_t now)
{
    SrvCap  pin;

    ed(_udpbuffer, (uint8_t*)&pin, sizeof(pin), __key, false);

    switch(pin._verb)
    {
    case PER_DATA:
        _data_in(s, bytes);
        _status = PER_LINKED;
        break;

    case PER_PING:
        std::cout << "PER  ->  PER PING FROM "<<Ip2str(s.Rsin())<<" \r\n";
        pin._verb = PER_PONG; //back
        s.send((const uint8_t*)&pin,sizeof(pin), s.Rsin(), __key);
        break;

    case PER_PONG:
        std::cout << "PER  -> PONG to (LINKED) << "<<  Ip2str(s.Rsin()) << " \n";
        _pers.insert(s.Rsin());
        _status = PER_LINKED;
        _show_pers(s);
        break;

    case SRV_PING:
        std::cout << "GOT SRV PING ?? NOT HERE ?!?!?" << IP2STR(s.Rsin()) << "\n";
        _pingtime = now;
        break;

    default:
        std::cout << "Invalid verb << "<< pin._verb <<"  form "<< Ip2str(s.Rsin()) << " \n";
        break;
    }
    return true;
}

/*
 */
void a_peer::_peering(udp_xdea& s, SrvCap&  plin)
{
    plin._verb = PER_PING;

    std::cout << "PUB_PUB  PER "<<__perport <<" PINGING->"<<  plin._u.pp._public.str() << "\r\n";
    s.send((const uint8_t*)&plin, sizeof(plin), plin._u.pp._public,__key);
}

bool a_peer::_data_in(udp_xdea& s, int bytes)
{
    //check if rSIn { _pers
    _udpbuffer[bytes]=0;
    std::cout << "DATA : [" << _udpbuffer+1 << "] from:" << Ip2str(s.Rsin()) << "\n";
    return true;
}

/**
 * @brief a_peer::snd
 * @param pd
 * @param l
 * @param crypt
 * @return
 */
void a_peer::send_to_pers(udp_xdea& s, const char* data, size_t len ,bool encrypt)
{
    for(const auto& p : _pers)
    {
        if(encrypt)
        {;}
        s.send((const uint8_t*)data,len,p);
    }
}

/**
 * @brief a_peer::rec
 * @param pd
 * @param l
 * @param decrypt
 * @return
 */
int  a_peer::rec(uint8_t* pd, size_t l, bool decrypt)
{
    UNUS(pd);
    UNUS(l);
    UNUS(decrypt);
    return 0;
}

void a_peer::_i_am_here(udp_xdea& s)
{
    return;
    _mecap._verb = PER_AIH;
    std::cout << "pinging server \r\n";
    s.send((const uint8_t*)&_mecap, sizeof(_mecap), _srvsin,__key);
}

void a_peer::_show_pers(udp_xdea& s)
{
    UNUS(s);
    std::cout << "------------------------------\n";
    for(const auto& p : _pers)
    {
        std::cout << " PERED " << IP2STR(p) << "\n";
    }
    std::cout << "------------------------------\n";
}

void a_peer::_proc_perrs(udp_xdea& s)
{
    UNUS(s);
    usleep(1000);
}



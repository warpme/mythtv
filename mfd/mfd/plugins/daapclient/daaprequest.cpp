/*
	daaprequest.h

	(c) 2003 Thor Sigvaldason and Isaac Richards
	Part of the mythTV project
	
    A little object for making daap requests

*/

#include <vector>
#include <iostream>
using namespace std;

#include "daaprequest.h"
#include "daapinstance.h"

DaapRequest::DaapRequest(
                            DaapInstance *owner,
                            const QString& l_base_url, 
                            const QString& l_host_address
                        )
{
    parent = owner;
    base_url = l_base_url;
    host_address = l_host_address;
}

void DaapRequest::addText(std::vector<char> *buffer, QString text_to_add)
{
    buffer->insert(buffer->end(), text_to_add.ascii(), text_to_add.ascii() + text_to_add.length());
}

bool DaapRequest::send(QSocketDevice *where_to_send)
{
    std::vector<char>  the_request;
    
    //
    //  Make the base url request (eg. /server-info)
    //
    
    QString top_line = QString("GET %1 HTTP/1.1\r\n").arg(base_url);
    addText(&the_request, top_line);
    
    //
    //  Add the server address (which the HTTP 1.1 spec is fairly adamant
    //  *must* be in there)
    // 
   
    QString host_line = QString("Host: %1\r\n").arg(host_address);
    addText(&the_request, host_line);

    //
    //  Add a few more "standard" daap headers (ie. things that iTunes sends
    //  when it is a client)
    //

    addText(&the_request, "Cache-Control: no-cache\r\n");
    addText(&the_request, "Accept: */*\r\n");

    /*
        Might want to add these at some point
        
        x-audiocast-udpport:49154
        icy-metadata:1
    */
    
    addText(&the_request, "User-Agent: iTunes/4.0 (Macintosh; N; PPC)\r\n");
    addText(&the_request, "Client-DAAP-Version: 1.0\r\n");

    //
    //  Add the final blank line
    //

    addText(&the_request, "\r\n");

    sendBlock(the_request, where_to_send);

    return true;
}

bool DaapRequest::sendBlock(std::vector<char> block_to_send, QSocketDevice *where_to_send)
{
    /*
    
        Debugging:
        
    
    
    cout << "=========== Debugging Output - DAAP request being sent  ==================" << endl;
    for(uint i = 0; i < block_to_send.size(); i++)
    {
        cout << block_to_send.at(i);
    }
    cout << "==========================================================================" << endl;
    
    */




    //
    //  May be overkill, but we do everything on select()'s in case the
    //  network is really slow/crappy.
    //

    int nfds = 0;
    fd_set writefds;
    struct  timeval timeout;
    int amount_written = 0;
    bool keep_going = true;

    //
    //  Could be that our payload (for example) is empty.
    //

    if(block_to_send.size() < 1)
    {
        warning("daap request was asked to sendBlock() "
                "of zero size ");
        keep_going = false;
    }


    while(keep_going)
    {
        if(!parent->keepGoing())
        {
            //
            //  time to escape out of this
            //

            parent->log("daap request aborted a sendBlock() "
                        "as it's time to go", 6);

            return false;
        }

        FD_ZERO(&writefds);
        FD_SET(where_to_send->socket(), &writefds);
        if(nfds <= where_to_send->socket())
        {
            nfds = where_to_send->socket() + 1;
        }
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int result = select(nfds, NULL, &writefds, NULL, &timeout);
        if(result < 0)
        {
            parent->warning("daap request got an error from "
                            "select() ... not sure what to do");
        }
        else
        {
            if(FD_ISSET(where_to_send->socket(), &writefds))
            {
                //
                //  Socket is available for writing
                //
            
                int bytes_sent = where_to_send->writeBlock( 
                                                            &(block_to_send[amount_written]), 
                                                            block_to_send.size() - amount_written
                                                          );
                if(bytes_sent < 0)
                {
                    //
                    //  Hmm, select() said we were ready, but now we're
                    //  getting an error ... server has gone away?
                    //
                    
                    parent->warning("daap request seems to have "
                                    "lost contact with the server "
                                    "... this is bad");
                    return false;
                
                }
                else if(bytes_sent >= (int) (block_to_send.size() - amount_written))
                {
                    //
                    //  All done
                    //

                    keep_going = false;
                }
                else
                {
                    amount_written += bytes_sent;
                }
            }
            else
            {
                //
                //  We just time'd out
                //
            }
        }
    }
    
    return true;
}

DaapRequest::~DaapRequest()
{
}

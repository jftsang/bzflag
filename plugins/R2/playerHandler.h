/* bzflag
* Copyright (c) 1993-2018 Tim Riker
*
* This package is free software;  you can redistribute it and/or
* modify it under the terms of the license found in the file
* named COPYING that should have accompanied this file.
*
* THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
*/

#ifndef _PLAYER_HANDLER_H_
#define _PLAYER_HANDLER_H_

#include "bzfsAPI.h"
#include "bzfsAPIServerSidePlayers.h"
#include <memory>

class PlayerHandler : public bz_ServerSidePlayerHandler
{
public:
    typedef std::shared_ptr<PlayerHandler> Ptr;

    virtual void added(int player);   // it is required that the bot provide this method

    virtual void textMessage(int dest, int source, const char *text);

    virtual void shotFired(int player, unsigned short shotID);

    void startPlay();

    virtual void spawned();
};

#endif //_PLAYER_HANDLER_H_


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4

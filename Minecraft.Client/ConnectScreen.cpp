#include "stdafx.h"
#include "ConnectScreen.h"
#include "ClientConnection.h"
#include "TitleScreen.h"
#include "Button.h"
#include "Minecraft.h"
#include "User.h"
#include "..\Minecraft.World\net.minecraft.locale.h"
#include "..\Minecraft.World\Socket.h"


ConnectScreen::ConnectScreen(Minecraft *minecraft, const wstring& ip, int port)
{
	aborted = false;
//    System.out.println("Connecting to " + ip + ", " + port);
    minecraft->setLevel(NULL);
#ifdef _WINDOWS64
	// Direct TCP connect to the dedicated server prototype.
	Socket *socket = new Socket(ip, port);
	connection = new ClientConnection(minecraft, socket);
#else
	// 4J - removed from separate thread, but need to investigate what we actually need here
    connection = new ClientConnection(minecraft, ip, port);
#endif
	if (connection == NULL || !connection->createdOk)
	{
		aborted = true;
		connection = NULL;
		return;
	}
    connection->send( shared_ptr<PreLoginPacket>( new PreLoginPacket(minecraft->user->name) ) );
}

void ConnectScreen::tick()
{
    if (connection != NULL)
	{
        connection->tick();
    }
}

void ConnectScreen::keyPressed(char eventCharacter, int eventKey)
{
}

void ConnectScreen::init()
{
    Language *language = Language::getInstance();

    buttons.clear();
    buttons.push_back(new Button(0, width / 2 - 100, height / 4 + 24 * 5 + 12, language->getElement(L"gui.cancel")));

}

void ConnectScreen::buttonClicked(Button *button)
{
    if (button->id == 0)
	{
        aborted = true;
        if (connection != NULL) connection->close();
        minecraft->setScreen(new TitleScreen());
    }
}

void ConnectScreen::render(int xm, int ym, float a)
{
    renderBackground();

    Language *language = Language::getInstance();

    if (connection == NULL)
	{
        drawCenteredString(font, language->getElement(L"connect.connecting"), width / 2, height / 2 - 50, 0xffffff);
        drawCenteredString(font, L"", width / 2, height / 2 - 10, 0xffffff);
    }
	else
	{
        drawCenteredString(font, language->getElement(L"connect.authorizing"), width / 2, height / 2 - 50, 0xffffff);
        drawCenteredString(font, connection->message, width / 2, height / 2 - 10, 0xffffff);
    }

    Screen::render(xm, ym, a);
}
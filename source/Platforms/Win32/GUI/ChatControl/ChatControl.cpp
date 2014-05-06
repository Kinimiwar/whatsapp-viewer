#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <iostream>
#include <fstream>

#include "ChatControl.h"
#include "ChatControlMessages/ChatControlMessage.h"
#include "ChatControlMessages/ChatControlMessageFrame.h"
#include "ChatControlMessages/ChatControlMessageImage.h"
#include "ChatControlMessages/ChatControlMessageLocation.h"
#include "ChatControlMessages/ChatControlMessageText.h"
#include "ChatControlMessages/ChatControlMessageVideo.h"
#include "AnimatedGif.h"
#include "DrawText.h"
#include "Smileys.h"
#include "../Counter.h"
#include "../ImageDecoder.h"
#include "../StringHelper.h"
#include "../Objects/Brush.h"
#include "../Objects/Font.h"
#include "../../../Exceptions/Exception.h"
#include "../../../Synchronization/Locked.h"
#include "../../../WhatsApp/Chat.h"
#include "../../../WhatsApp/Message.h"
#include "../../../UTF8/utf8.h"
#include "../../../Log.h"
#include "../../../VectorUtils.h"
#include "../../../../resources/resource.h"

ChatControl::ChatControl(HWND window)
{
	imageDecoder = new ImageDecoder();
	smileys = new Smileys(*imageDecoder);
	loadingAnimation = new AnimatedGif(L"IDR_LOADING", L"GIF", *imageDecoder);
	this->window = window;
	dateFont = new Font(CreateFont(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Courier New"));
	chat = NULL;
	shouldResizeMessages = true;
	buildingMessagesThreadHandle = NULL;
	resizingMessagesThreadHandle = NULL;
	loadingAnimationThreadHandle = NULL;
	buildingMessages = false;
	resizingMessages = false;
	painting = false;
	renderingLoadingAnimation = false;
	totalMessagesHeight = 0;
}

ChatControl::~ChatControl()
{
	stopResizingMessages();
	stopBuildingMessages();
	stopLoadingAnimation();

	clearMessages();
	delete dateFont;
	delete loadingAnimation;
	delete smileys;
	delete imageDecoder;
}

void ChatControl::registerChatControl()
{
	WNDCLASSEX windowClass;
	memset(&windowClass, 0, sizeof(WNDCLASSEX));

	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.lpszClassName = L"ChatControl";
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
	windowClass.style = 0;
	windowClass.lpfnWndProc = ChatControlCallback;
	windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	windowClass.cbWndExtra = sizeof(ChatControl *);
	RegisterClassEx(&windowClass);
}

void ChatControl::setChat(WhatsappChat &chat)
{
	deleteBackbuffer();
	this->chat = &chat;
	startBuildingMessages();
	totalMessagesHeight = 0;
	calculateScrollInfo();
	redraw();
}

void ChatControl::startBuildingMessages()
{
	painting = false;
	stopResizingMessages();
	stopBuildingMessages();
	buildingMessages = true;
	buildingMessagesThreadHandle = CreateThread(NULL, 0, buildingMessagesThread, this, 0, NULL);
}

void ChatControl::stopBuildingMessages()
{
	if (buildingMessagesThreadHandle)
	{
		buildingMessages = false;
		WaitForSingleObject(buildingMessagesThreadHandle, INFINITE);
		buildingMessagesThreadHandle = NULL;
	}
}

DWORD CALLBACK ChatControl::buildingMessagesThread(void *param)
{
	ChatControl *chatControl = reinterpret_cast<ChatControl *>(param);
	chatControl->buildMessages();
	PostMessage(chatControl->window, WM_CHATCONTROL, CHAT_CONTROL_BUILDING_MESSAGES_FINISHED, 0);
	return 0;
}

void ChatControl::buildMessages()
{
	if (!lock.tryLockWhile(buildingMessages))
	{
		return;
	}

	clearMessages();

	if (chat != NULL)
	{
		std::vector<WhatsappMessage *> &messages = chat->getMessages(buildingMessages);
		for (std::vector<WhatsappMessage *>::iterator it = messages.begin(); it != messages.end(); ++it)
		{
			if (!buildingMessages)
			{
				break;
			}

			WhatsappMessage &message = **it;
			ChatControlMessageFrame *messageFrame = NULL;

			int color;

			if (message.isFromMe())
			{
				color = RGB(190, 240, 150);
			}
			else
			{
				color = RGB(230, 230, 240);
			}

			switch (message.getMediaWhatsappType())
			{
				case MEDIA_WHATSAPP_TEXT:
				{
					messageFrame = new ChatControlMessageFrame(new ChatControlMessageText(message, 0, *smileys), 0, color, *dateFont);
				} break;
				case MEDIA_WHATSAPP_IMAGE:
				{
					if (message.getRawDataSize() > 0 && message.getRawData() != NULL)
					{
						messageFrame = new ChatControlMessageFrame(new ChatControlMessageImage(message, 0, *imageDecoder), 0, color, *dateFont);
					}
				} break;
				case MEDIA_WHATSAPP_VIDEO:
				{
					if (message.getRawDataSize() > 0 && message.getRawData() != NULL)
					{
						messageFrame = new ChatControlMessageFrame(new ChatControlMessageVideo(message, 0, *imageDecoder), 0, color, *dateFont);
					}
				} break;
				case MEDIA_WHATSAPP_LOCATION:
				{
					messageFrame = new ChatControlMessageFrame(new ChatControlMessageLocation(message, 0, *imageDecoder), 0, color, *dateFont);
				} break;
			}

			if (messageFrame != NULL)
			{
				this->messages.push_back(messageFrame);
			}
		}
	}

	lock.unlock();
}

void ChatControl::startResizingMessages()
{
	painting = false;
	stopResizingMessages();
	resizingMessages = true;
	resizingMessagesThreadHandle = CreateThread(NULL, 0, resizingMessagesThread, this, 0, NULL);

	redraw();
}

void ChatControl::stopResizingMessages()
{
	if (buildingMessagesThreadHandle)
	{
		resizingMessages = false;
		WaitForSingleObject(resizingMessagesThreadHandle, INFINITE);
		resizingMessagesThreadHandle = NULL;
	}
}

DWORD CALLBACK ChatControl::resizingMessagesThread(void *param)
{
	ChatControl *chatControl = reinterpret_cast<ChatControl *>(param);
	chatControl->resizeMessages();
	PostMessage(chatControl->window, WM_CHATCONTROL, CHAT_CONTROL_RESIZING_MESSAGES_FINISHED, 0);
	return 0;
}

void ChatControl::resizeMessages()
{
	if (!lock.tryLockWhile(resizingMessages))
	{
		return;
	}

	resizeMessageWidths();
	lock.unlock();
}

void ChatControl::resizeMessageWidths()
{
	RECT clientRect;
	GetClientRect(window, &clientRect);

	int y = 40;
	int gap = 40;
	int width = clientRect.right - clientRect.left - 20 - gap;

	for (std::vector<ChatControlMessageFrame *>::iterator it = messages.begin(); it != messages.end(); ++it)
	{
		if (!resizingMessages)
		{
			return;
		}

		ChatControlMessageFrame &messageFrame = **it;
		messageFrame.updateWidth(window, width);
		y += 8 + messageFrame.getHeight();
	}

	totalMessagesHeight = y;
}

DWORD CALLBACK ChatControl::loadingAnimationThread(void *param)
{
	ChatControl *chatControl = reinterpret_cast<ChatControl *>(param);
	chatControl->loadingAnimationLoop();
	return 0;
}

void ChatControl::loadingAnimationLoop()
{
	int frameCount = loadingAnimation->getFrameCount();

	while (renderingLoadingAnimation)
	{
		for (int frameIndex = 0; frameIndex < frameCount; frameIndex++)
		{
			if (!renderingLoadingAnimation)
			{
				break;
			}

			RECT clientRect;
			GetClientRect(window, &clientRect);

			int x = (clientRect.right - 128) / 2;
			int y = (clientRect.bottom - 128) / 2;

			HDC deviceContext = GetDC(window);
			loadingAnimation->renderFrame(deviceContext, frameIndex, x, y);
			ReleaseDC(window, deviceContext);

			Sleep(120);
		}
	}
}

void ChatControl::startLoadingAnimation()
{
	stopLoadingAnimation();
	renderingLoadingAnimation = true;
	loadingAnimationThreadHandle = CreateThread(NULL, 0, loadingAnimationThread, this, 0, NULL);
}

void ChatControl::stopLoadingAnimation()
{
	if (loadingAnimationThreadHandle)
	{
		renderingLoadingAnimation = false;
		WaitForSingleObject(loadingAnimationThreadHandle, INFINITE);
		loadingAnimationThreadHandle = NULL;
	}
}

void ChatControl::clearMessages()
{
	clearVector(messages);
}

void ChatControl::calculateScrollInfo()
{
	RECT clientRect;
	GetClientRect(window, &clientRect);

	SCROLLINFO scrollInfo;
	memset(&scrollInfo, 0, sizeof(SCROLLINFO));
	scrollInfo.cbSize = sizeof(SCROLLINFO);
	scrollInfo.fMask = SIF_PAGE | SIF_RANGE;
	scrollInfo.nMin = 0;
	scrollInfo.nMax = totalMessagesHeight;
	scrollInfo.nPage = clientRect.bottom;

	SetScrollInfo(window, SB_VERT, &scrollInfo, TRUE);
}

void ChatControl::deleteBackbuffer()
{
	DeleteObject(backbufferBitmap);
	backbufferBitmap = NULL;

	DeleteDC(backbuffer);
	backbuffer = NULL;
}

void ChatControl::createBackbuffer()
{
	deleteBackbuffer();

	RECT clientRect;
	GetClientRect(window, &clientRect);

	HDC deviceContext = GetDC(window);

	backbuffer = CreateCompatibleDC(deviceContext);
	backbufferBitmap = CreateCompatibleBitmap(deviceContext, clientRect.right, clientRect.bottom);
	SelectObject(backbuffer, backbufferBitmap);

	ReleaseDC(window, deviceContext);

	paintBackbuffer();
}

void ChatControl::paintBackbuffer()
{
	HANDLE oldFont = SelectObject(backbuffer, GetStockObject(DEFAULT_GUI_FONT));
	SetTextColor(backbuffer, RGB(0, 0, 0));
	SetBkColor(backbuffer, GetSysColor(COLOR_3DFACE));

	RECT clientRect;
	GetClientRect(window, &clientRect);

	Brush brush(CreateSolidBrush(GetSysColor(COLOR_3DFACE)));
	FillRect(backbuffer, &clientRect, brush.get());

	if (lock.tryLock())
	{
		painting = true;
		int scrollPosition = GetScrollPos(window, SB_VERT);

		int y = 10;
		if (y - scrollPosition > 0)
		{
			std::string text = "WhatsApp Chat";
			if (chat != NULL)
			{
				text += " (";
				text += chat->getKey();

				if (chat->getSubject().length() > 0)
				{
					text += "; ";
					text += chat->getSubject();
				}

				text += ")";
			}

			WCHAR *wcharText = buildWcharString(text);
			drawText(backbuffer, wcharText, 10, 10, clientRect.right - 10);
			delete[] wcharText;
		}

		y += 15;
		if (y - scrollPosition)
		{
			MoveToEx(backbuffer, 10, y - scrollPosition, NULL);
			LineTo(backbuffer, clientRect.right - 10, y - scrollPosition);
		}

		y += 15;

		if (chat != NULL)
		{
			for (std::vector<ChatControlMessageFrame *>::iterator it = messages.begin(); it != messages.end(); ++it)
			{
				if (!painting)
				{
					break;
				}

				ChatControlMessageFrame &messageFrame = **it;

				int x = 10;

				if (messageFrame.getMessage()->getMessage().isFromMe())
				{
					x += 40;
				}

				if (y + messageFrame.getHeight() - scrollPosition > 0)
				{
					messageFrame.render(backbuffer, x, y - scrollPosition, clientRect.bottom);
				}
				y += messageFrame.getHeight();
				y += 8;

				if (y - scrollPosition > clientRect.bottom)
				{
					break;
				}
			}
		}

		painting = false;
		lock.unlock();
	}
	else
	{
		drawTextCentered(backbuffer, L"Loading...", 10, (clientRect.bottom + 128) / 2 + 30, clientRect.right - 10);
		startLoadingAnimation();
	}

	SelectObject(backbuffer, oldFont);
}

LRESULT ChatControl::onPaint()
{
	if (!backbuffer)
	{
		createBackbuffer();
	}

	HDC deviceContext;
	PAINTSTRUCT paint;

	RECT clientRect;
	GetClientRect(window, &clientRect);

	deviceContext = BeginPaint(window, &paint);
	BitBlt(deviceContext, 0, 0, clientRect.right, clientRect.bottom, backbuffer, 0, 0, SRCCOPY);
	EndPaint(window, &paint);

	return 0;
}

void ChatControl::redraw()
{
	InvalidateRect(window, NULL, FALSE);
	// UpdateWindow(window);
}

void ChatControl::scroll(int newPosition)
{
	int previousPosition = GetScrollPos(window, SB_VERT);

	SCROLLINFO scrollInfo;
	memset(&scrollInfo, 0, sizeof(SCROLLINFO));
	scrollInfo.cbSize = sizeof(SCROLLINFO);
	scrollInfo.fMask = SIF_POS;
	scrollInfo.nPos = newPosition;

	SetScrollInfo(window, SB_VERT, &scrollInfo, TRUE);
	GetScrollInfo(window, SB_VERT, &scrollInfo);

	if (scrollInfo.nPos != previousPosition)
	{
		// ScrollWindow(hwnd, 0, yChar * (yPos - si.nPos), NULL, NULL);
		// UpdateWindow (hwnd);
		paintBackbuffer();
		redraw();
	}
}

LRESULT ChatControl::onScroll(WPARAM wParam)
{
	SCROLLINFO scrollInfo;
	memset(&scrollInfo, 0, sizeof(SCROLLINFO));
	scrollInfo.cbSize = sizeof(SCROLLINFO);
	scrollInfo.fMask = SIF_ALL;
	GetScrollInfo(window, SB_VERT, &scrollInfo);

	int previousPosition = scrollInfo.nPos;

	switch (LOWORD(wParam))
	{
		case SB_TOP:
		{
			scrollInfo.nPos = scrollInfo.nMin;
		} break;
		case SB_BOTTOM:
		{
			scrollInfo.nPos = scrollInfo.nMax;
		};
		case SB_LINEUP:
		{
			scrollInfo.nPos -= 30;
		} break;
		case SB_LINEDOWN:
		{
			scrollInfo.nPos += 30;
		} break;
		case SB_PAGEUP:
		{
			scrollInfo.nPos -= scrollInfo.nPage;
		} break;
		case SB_PAGEDOWN:
		{
			scrollInfo.nPos += scrollInfo.nPage;
		} break;
		case SB_THUMBTRACK:
		{
			scrollInfo.nPos = scrollInfo.nTrackPos;
		} break;
	}

	scroll(scrollInfo.nPos);
	return 0;
}

LRESULT ChatControl::onMousewheel(int delta)
{
	int previousPosition = GetScrollPos(window, SB_VERT);
	scroll(previousPosition - delta / WHEEL_DELTA * 60);

	return 0;
}

LRESULT CALLBACK ChatControl::ChatControlCallback(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	ChatControl *chatControl = reinterpret_cast<ChatControl *>(GetWindowLongPtr(window, 0));

	try
	{
		switch (message)
		{
			case WM_NCCREATE:
			{
				RECT rect;
				GetClientRect(window, &rect);

				try
				{
					chatControl = new ChatControl(window);
				}
				catch (Exception &exception)
				{
					std::wstring cause = strtowstr(exception.getCause());
					MessageBox(NULL, cause.c_str(), L"Error", MB_OK | MB_ICONERROR);
					return FALSE;
				}

				SetWindowLongPtr(window, 0, reinterpret_cast<LONG>(chatControl));
				ShowScrollBar(window, SB_VERT, FALSE);
			} break;
			case WM_CREATE:
			{
				if (!chatControl)
				{
					return -1;
				}

				chatControl->createBackbuffer();
			} break;
			case WM_PAINT:
			{
				return chatControl->onPaint();
			} break;
			case WM_ERASEBKGND:
			{
				return 1;
			} break;
			case WM_VSCROLL:
			{
				return chatControl->onScroll(wParam);
			} break;
			case WM_MOUSEACTIVATE:
			{
				SetFocus(window);
				return MA_ACTIVATE;
			} break;
			case WM_MOUSEWHEEL:
			{
				return chatControl->onMousewheel(GET_WHEEL_DELTA_WPARAM(wParam));
			} break;
			case WM_KEYDOWN:
			{
				switch (wParam)
				{
					case VK_DOWN:
					{
						SendMessage(window, WM_VSCROLL, MAKELONG(SB_LINEDOWN, 0), 0);
						return 0;
					} break;
					case VK_UP:
					{
						SendMessage(window, WM_VSCROLL, MAKELONG(SB_LINEUP, 0), 0);
						return 0;
					} break;
					case VK_HOME:
					{
						SendMessage(window, WM_VSCROLL, MAKELONG(SB_TOP, 0), 0);
						return 0;
					} break;
					case VK_END:
					{
						SendMessage(window, WM_VSCROLL, MAKELONG(SB_BOTTOM, 0), 0);
						return 0;
					} break;
					case VK_PRIOR:
					{
						SendMessage(window, WM_VSCROLL, MAKELONG(SB_PAGEUP, 0), 0);
						return 0;
					} break;
					case VK_NEXT:
					{
						SendMessage(window, WM_VSCROLL, MAKELONG(SB_PAGEDOWN, 0), 0);
						return 0;
					} break;
				}
			 } break;
			case WM_GETDLGCODE:
			{
				return DLGC_WANTARROWS;
			} break;
			case WM_CHATCONTROL:
			{
				switch (wParam)
				{
					case CHAT_CONTROL_SETCHAT:
					{
						chatControl->setChat(*reinterpret_cast<WhatsappChat *>(lParam));
					} break;
					case CHAT_CONTROL_START_RESIZING_MESSAGES:
					{
						chatControl->shouldResizeMessages = true;
					} break;
					case CHAT_CONTROL_STOP_RESIZING_MESSAGES:
					{
						chatControl->shouldResizeMessages = false;
					} break;
					case CHAT_CONTROL_REDRAW:
					{
						chatControl->startResizingMessages();
						chatControl->createBackbuffer();
						chatControl->redraw();
					} break;
					case CHAT_CONTROL_RESIZING_MESSAGES_FINISHED:
					{
						chatControl->resizingMessages = false;
						chatControl->resizingMessagesThreadHandle = NULL;
						chatControl->stopLoadingAnimation();
						chatControl->calculateScrollInfo();
						chatControl->createBackbuffer();
						chatControl->redraw();
					} break;
					case CHAT_CONTROL_BUILDING_MESSAGES_FINISHED:
					{
						chatControl->buildingMessages = false;
						chatControl->buildingMessagesThreadHandle = NULL;
						chatControl->stopLoadingAnimation();
						chatControl->startResizingMessages();
					} break;
				}
			} break;
			case WM_SIZE:
			{
				if (chatControl->shouldResizeMessages)
				{
					chatControl->startResizingMessages();
				}
				chatControl->createBackbuffer();
				chatControl->redraw();
			} break;
			case WM_NCDESTROY:
			{
				delete chatControl;
			} break;
		}
	}
	catch (Exception &exception)
	{
		std::wstring cause = strtowstr(exception.getCause());
		MessageBox(NULL, cause.c_str(), L"Error", MB_OK | MB_ICONERROR);
		return DefWindowProc(window, message, wParam, lParam);
	}

	return DefWindowProc(window, message, wParam, lParam);
}
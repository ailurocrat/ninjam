#include "precomp.h"

#include "resource.h"

#include <bfc/attrib/attribs.h>
#include <bfc/std_wnd.h>
#include <parse/pathparse.h>
#include <wnd/wnds/os/osdialog.h>
#include <wnd/wnds/os/osdockwnd.h>

#include "../audiostream_asio.h"
#include "../njclient.h"

#include "attrs.h"

#include "audio.h"
#include "njrackwnd.h"
#include "chatwnd.h"
#include "jesus.h"
#include "session.h"
#include "settings.h"
#include "version.h"

#include "mainwnd.h"

#define ANONYMOUS "anonymous"

#define MAX_PREV_SERVERS 12

enum {
  CMD_CONNECT,
  CMD_DISCONNECT,
  CMD_EXIT,
  CMD_ADDLOCAL,
  CMD_KILLDEADCHANNELS,
  CMD_ASIO_CFG,
  CMD_SHOW_JESUS,
  CMD_SHOW_CHAT,
  CMD_SETTINGS,
  CMD_ABOUT,
};

enum {
  TIMER_CHECK_CONNECTION,
  TIMER_RUN_CLIENT,
  TIMER_UPDATE_USERINFO,
};

static ChatWnd *chatwnd;

extern MainWnd *mainw;

//app jesusonic stuff
HWND jesuswnd;
int jesus_showing;

NJClient *g_client;

//CUT extern char *get_asio_configstr(char *inifile, int wantdlg, HWND par);
#ifdef _WIN32
audioStreamer *CreateConfiguredStreamer(char *inifile, int showcfg, HWND hwndParent);
#endif
audioStreamer *g_audio;
String g_server_address, g_user_name, g_passwd;
int g_audio_enable=0;
static int in_g_client_run=0;

void audiostream_onunder() { }
void audiostream_onover() { }

//makes a username safe to touch
String safeify(const char *s) {
  String ret = s;
  for (char *p = ret.ncv(); *p; p++) {
    if (!ISDIGIT(*p) && !ISALPHA(*p) && !ISSPACE(*p) && *p != '@' && *p != '-' && *p != '_' && *p != '.') *p = '_';
  }
  return ret;
}

int displayLicense(int user32, char *licensetext) {
  String licky(licensetext);
  licky.replace("\n", "\r\n");
  _string txt;
  txt = licky;
  OSDialog lic(IDD_SERVER_LICENSE, reinterpret_cast<HWND>(user32));
  lic.registerAttribute(txt, IDC_SERVER_LICENSE_TEXT);
  _bool agree;
  lic.registerAttribute(agree, IDC_SERVER_LICENSE_AGREE);
  lic.registerBoolDisable(agree, IDOK, FALSE);
  lic.registerBoolDisable(agree, IDCANCEL, TRUE);
  lic.setFocusedControl(IDC_SERVER_LICENSE_AGREE);//FUCKO doesn't work?
  return lic.createModal();
}

void audiostream_onsamples(float **inbuf, int innch, float **outbuf, int outnch, int len, int srate)
{ 
  if (!g_audio_enable) 
  {
    int x;
    // clear all output buffers
    for (x = 0; x < outnch; x ++) memset(outbuf[x],0,sizeof(float)*len);
    return;
  }
  if (g_client) {
#if 0
    if (g_client->IsAudioRunning())
    {
      if (JesusonicAPI && myInst)
      {
        _controlfp(_RC_CHOP,_MCW_RC);
        JesusonicAPI->jesus_process_samples(myInst,(char*)buf,len*nch*sizeof(float));
        JesusonicAPI->osc_run(myInst,(char*)buf,len*nch*sizeof(float));
      }
    }
#endif
    g_client->AudioProc(inbuf,innch, outbuf, outnch, len,srate);
  }
}

//CUT static char *dev_name_in;

void audioStop() {
  g_audio_enable=0;
  delete g_audio; g_audio = NULL;
}

int audioStart() {
  audioStop();
  g_audio=CreateConfiguredStreamer("ninjam.ini", FALSE, NULL);
  return (g_audio != NULL);
}

// just config it
int audioConfig() {
  int audio_was_running = 0;
  if (g_audio) {
    audio_was_running = 1;
    int r = StdWnd::messageBox("Session is in progress. You must disconnect if you wish to change your audio settings. Click OK to disconnect and open the configuration dialog or Cancel to continue with things are they are.",
      "Cannot change audio during session",
      MB_OKCANCEL|MB_TASKMODAL);
    if (r == IDCANCEL) return 0;
    mainw->handleDisconnect();
  }
    
  audioStop();

  extern MainWnd *mainw;
  CreateConfiguredStreamer("ninjam.ini", -1, mainw->getOsWindowHandle());

  return 1;
}

MainWnd::MainWnd() {
  setIcon(IDI_NINJAM);
  setFormattedName();
  rackwnd = new NJRackWnd;
  ASSERT(g_audio == NULL);
  ASSERT(g_client == NULL);
  g_client = new NJClient;	// FUCKO should be in app.cpp probably

  g_client->config_autosubscribe = 1;
  g_client->config_savelocalaudio = 1;

  g_client->config_metronome_mute = metronome_was_muted;
}

MainWnd::~MainWnd() {
  metronome_was_muted = g_client->config_metronome_mute;

  delete chatwnd; chatwnd = NULL;

  delete rackwnd;	// not sure who should

  delete g_audio;

// delete effects processors
  for (int i = 0; ; i++) {
    int a=g_client->EnumLocalChannels(i);
    if (a<0) break;
    void *inst=0;
    g_client->GetLocalChannelProcessor(a,NULL,&inst);
    if (inst) deleteJesusonicProc(inst,a);
    g_client->SetLocalChannelProcessor(a,NULL,NULL);
  }

  if (g_client != NULL) {
    delete g_client->waveWrite;
    g_client->waveWrite=0;
    delete g_client; g_client = NULL;
  }

  // shh jesus is sleeping
  jesusShutdown();
}

void MainWnd::setFormattedName(const char *substr) {
  StringPrintf name("%s %s", APPNAME, VERSION);
  String sub(substr);
  if (sub.notempty()) name += StringPrintf(" - %s", sub.vs());
  setName(name);
}

int MainWnd::onInit() {
  int r = MAINWND_PARENT::onInit();

  g_client->LicenseAgreement_User32=reinterpret_cast<int>(getOsWindowHandle());
  g_client->LicenseAgreementCallback=displayLicense;

  audioConfig();	//FUCKO get rid of this

  // jesusonic init
  jesusInit();

// create our rack-- has to be after asio init
  rackwnd->init(this);

// menus

//  PopupMenu *p = new PopupMenu;
//  p->addCommand("blah", -1);
//  addMenu("&File", 1, p);

  PopupMenu *p = new PopupMenu;
  p->addCommand("&Connect to server...", CMD_CONNECT);
  //CUTp->addCommand("Disconnect", CMD_DISCONNECT, FALSE, TRUE);
  p->addCommand("&Disconnect", CMD_DISCONNECT);
  p->addSeparator();
  p->addCommand("&Exit", CMD_EXIT);
  addMenu("&Connection", 1, p);

  p = new PopupMenu;
  p->addCommand("&Add local channel", CMD_ADDLOCAL);
//  p->addCommand("&Remove dead channels", CMD_KILLDEADCHANNELS);

// remove local
// remove dead inbound channels
  addMenu("C&hannels", 2, p);

  p = new PopupMenu;
  p->addCommand("&Settings...", CMD_SETTINGS);
  p->addCommand("&Audio configuration...", CMD_ASIO_CFG);
  addMenu("&Options", 4, p);

  p = new PopupMenu;
  p->addCommand("&About...", CMD_ABOUT);
  addMenu("&Help", 5, p);

  setStatusBar(TRUE);
  OSStatusBarWnd *statusbar = getStatusBarWnd();
  StatusBarPart *statusline = statusbar->addNewPart();
//  statusbar->addNewPart();


// timers
  timerclient_setTimer(TIMER_CHECK_CONNECTION, 500);
  timerclient_timerCallback(TIMER_CHECK_CONNECTION);
  timerclient_setTimer(TIMER_RUN_CLIENT, 50);	// NJClient->Run()
//CUT  timerclient_setTimer(TIMER_UPDATE_USERINFO, 1000);

// chat
  if (chatwnd == NULL) {
    OSDockWndHoster *hoser = new OSDockWndHoster();
    hoser->setName("Chat dock");
    hoser->setDockedWidth(200);
    hoser->setDockedHeight(140);
    //hoser->setDockResizable(FALSE);
    hoser->setDockResizable(TRUE);
    hoser->setBorderVisible(FALSE);
    dockWindow(hoser, DOCK_RIGHT);

    chatwnd = new ChatWnd(hoser->getOsWindowHandle());
    chatwnd->createModeless(FALSE, FALSE);
    hoser->setWindowHandle(chatwnd->getOsWindowHandle());
    StdWnd::showWnd(chatwnd->getOsWindowHandle());

    StdWnd::setFocus(getOsWindowHandle());
  }

  return r;
}

int MainWnd::onResize() {
  MAINWND_PARENT::onResize();

  if (rackwnd) rackwnd->resizeToRect(&clientRect());

  return 1;
}

int MainWnd::onMenuCommand(int cmd) {
  switch (cmd) {
    case CMD_CONNECT: {
      handleConnect();
    }
    break;
    case CMD_DISCONNECT: {
      handleDisconnect();
    }
    break;
    case CMD_EXIT: {
      onUserClose();
    }
    break;
    case CMD_ADDLOCAL: {
      rackwnd->addLocalChannel();
    }
    break;
    case CMD_KILLDEADCHANNELS: {
      rackwnd->killDeadChannels();
    }
    break;
    case CMD_SHOW_JESUS: {
      if (jesuswnd == NULL) break;
      if (!jesus_showing) {
        ShowWindow(jesuswnd, SW_SHOWNA);
        jesus_showing = 1;
      } else {
        ShowWindow(jesuswnd, SW_HIDE);
        jesus_showing = 0;
      }
    }
    break;
    case CMD_SHOW_CHAT: {
      ASSERT(chatwnd != NULL);
      StdWnd::showWnd(chatwnd->getOsWindowHandle());
      chat_was_showing = 1;
    }
    break;
    case CMD_SETTINGS: {
      Settings(getOsWindowHandle()).createModal();
    }
    break;
    case CMD_ASIO_CFG: {
      audioConfig();
    }
    break;
    case CMD_ABOUT: {
      OSDialog dlg(IDD_ABOUT, getOsWindowHandle());
      _string vstr;
      vstr = StringPrintf("Version %s", VERSION);
      dlg.registerAttribute(vstr, IDC_VERSION_TEXT);
      dlg.createModal();
    }
    break;
    default:
      MAINWND_PARENT::onMenuCommand(cmd);
      return 0;
  }
  return 1;
}

void MainWnd::onUserClose() {
  PostQuitMessage(0);
}

void MainWnd::timerclient_timerCallback(int id) {
  switch (id) {
    case TIMER_CHECK_CONNECTION: {
      String status_text="";
      if (g_client == NULL) break;	// no client
      int r = g_client->GetStatus();
//CUT DebugString("status: %d", r);
      String err = g_client->GetErrorStr();
      if (r != NJClient::NJC_STATUS_OK && err.notempty()) {
        status_text = StringPrintf("Status: %s", err.vs());
      }
      else
      switch (r) {
        case NJClient::NJC_STATUS_DISCONNECTED:
          status_text = "Disconnected.";
          g_audio_enable = 0;
        break;
        case NJClient::NJC_STATUS_INVALIDAUTH:
          status_text = "Error: Can't connect to server: Invalid authorization.";
        break;
        case NJClient::NJC_STATUS_CANTCONNECT:
          status_text = "Error: Can't connect to server.";
        break;
        case NJClient::NJC_STATUS_PRECONNECT:
          status_text = "Not connected.";
        break;
        case NJClient::NJC_STATUS_OK:
          status_text.printf("Connected to %s (%s) %3.0f BPM, %d BPI", g_client->GetHostName(),
            g_client->GetUserName(),
            g_client->GetActualBPM(),
            g_client->GetBPI()
          );
        break;
//CUT        case NJClient::NJC_STATUS_RECONNECTING:
//CUT          status_text = "Reconnecting...";
//CUT        break;
      }

      OSStatusBarWnd *statusbar = getStatusBarWnd();
      StatusBarPart *statusline = statusbar->enumPart(0);
      statusline->setText(status_text);
    }
    break;
    case TIMER_RUN_CLIENT: {
       if (!g_client) break;
       if (g_client && !in_g_client_run) {
         in_g_client_run = 1;
#if 0
         stdtimevalms then = Std::getTimeStampMS();
         while (Std::getTimeStampMS() - then < .1f) {
           if (g_client->Run()) break;
         }
#else
//      if (g_client->GetStatus() != NJClient::NJC_STATUS_OK) 
         while (!g_client->Run()) { }
#endif
         in_g_client_run = 0;
       }
//    }
//    break;
//    case TIMER_UPDATE_USERINFO: {

      if (g_client == NULL || g_client->HasUserInfoChanged() == 0) break;

      rackwnd->killDeadChannels();	//FUCKO, just mark em and remove later

      if (g_client->GetStatus() != NJClient::NJC_STATUS_OK) break;

      // update list of channels
      DebugString("\nUser, channel list:\n");
      for (int i = 0; ; i++) {
        char *usernamept = g_client->GetUserState(i);
        if (usernamept == NULL) break;
        String un=safeify(usernamept);
        if (un.isempty()) break;

        DebugString(" %s\n",un.v());

        for (int ch = 0; ; ch++) {
          int id = g_client->EnumUserChannels(i, ch);
          if (id < 0) break;
DebugString("\tchannel id %d\n", id);

          // just add to rack, only new ones will be added
          rackwnd->addChannel(un, id);
        }

#if 0
        int ch=0;
        for (;;) {
          bool sub;
          char *cn=g_client->GetUserChannelState(us,ch,&sub);
          if (!cn) break;
          DebugString("    %d: \"%s\" subscribed=%d\n",ch,cn,sub?1:0);


          ch++;
        }
#endif
//CUT        us++;
      }
      rackwnd->refreshChannels();

//CUT      if (!us) DebugString("  <no users>\n");
    }
    break;
  }
}

void MainWnd::handleConnect() {
  handleDisconnect();

//FUCKO disconnect if connected, maybe pop up box
      OSDialog dlg(IDD_USERPASS, getOsWindowHandle());
      _string server("Last server"), username("Last username"), passwd("Last password");
      _bool remember_passwd("Remember password"), anon("Login anonymously", TRUE);

      dlg.registerAttribute(server, IDC_SERVER);

      PathParser pp(prev_servers, "|");
      PtrList<String> prevs = pp.makeStringList();
      foreach_reverse(prevs)
        dlg.populateCombo(IDC_SERVER, prevs.getfor()->v());
      endfor

      dlg.registerAttribute(username, IDC_USERNAME);
      dlg.registerAttribute(passwd, IDC_PASSWD);
      dlg.registerAttribute(remember_passwd, IDC_REMEMBER_PASSWD);
      dlg.registerAttribute(anon, IDC_ANONYMOUS);

//CUT      dlg.registerBoolDisable(anon, IDC_USERNAME, TRUE);
      dlg.registerBoolDisable(anon, IDC_PASSWD, TRUE);
      dlg.registerBoolDisable(anon, IDC_REMEMBER_PASSWD, TRUE);

      if (dlg.createModal() && server.notempty()) {

        // modify the recent servers list, most recent at end
//CUT        int hadit = 0;
        foreach(prevs)
          if (prevs.getfor()->iscaseequal(server)) {
            delete prevs.getfor();
            prevs.removeByPos(foreach_index);
//CUT            prevs.moveItem(foreach_index, PTRLIST_POS_LAST);	// move to end
//CUT            hadit = 1;
            break;
          }
        endfor
//        if (!hadit) {
          if (prevs.n() > MAX_PREV_SERVERS) {
            delete prevs[0];	// kill least recently used
            prevs.removeByPos(0);
          }
          prevs += new String(server);	// add on end
//        }
        int needpipe=0;
        String concat;
        foreach(prevs)
          if (needpipe) concat += "|";
          String *s = prevs.getfor();
          ASSERT(s != NULL);
          concat += s->v();
          needpipe=1;
        endfor

        prev_servers = concat;	// save off prev server list

        // handle anonymity weirdness
        String connect_username = anon ? StringPrintf("%s:%s", ANONYMOUS, username.v()) : String(username);
        String connect_passwd = anon ? "" : passwd;

        // save off given values
        g_server_address = server;
        g_user_name = username;
        g_passwd = passwd;

        if (!remember_passwd) passwd = "";	// don't remember

        if (!audioStart()) { // gee I hope the config is ok
          StdWnd::messageBox(
            "The audio device could not be opened. Maybe it's in use already? Or you"
            " can try turning it off and on.",
            "Error opening audio device",
            MB_OK|MB_TASKMODAL);
        } else {

          g_client->Connect(g_server_address.ncv(), connect_username.ncv(), connect_passwd.ncv());

          g_audio_enable=1;

          rackwnd->onConnect();

          //begin a new session
          Session::newSession();
        }
      }
}

void MainWnd::handleDisconnect() {
  if (g_client) {
    audioStop();
    rackwnd->onDisconnect();
    Session::endSession();	// disconnects g_client etc
    chatwnd->addChatLine(NULL, "*** Disconnected from server.");
  }
}

LRESULT MainWnd::wndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_WINDOWPOSCHANGED: {
      WINDOWPOS *wp = (WINDOWPOS*)lParam;
      if (!(wp->flags & SWP_NOMOVE)) {
        mainwnd_x = wp->x;
        mainwnd_y = wp->y;
//CUT DebugString("pos change");
      }
      if (!(wp->flags & SWP_NOSIZE)) {
        mainwnd_w = wp->cx;
        mainwnd_h = wp->cy;
//CUT DebugString("size change");
      }
    }
    break;
  }
  return MAINWND_PARENT::wndProc(hWnd, uMsg, wParam, lParam);
}

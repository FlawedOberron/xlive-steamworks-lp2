// LP2 mods: a sixth Options entry, Network, below the game's five (flash\option\id500_PC). Its
// panel is a copy of the Controls panel (mc_subMenu1) showing four rows. The ring menu's own
// script (mc_main, frame 15) drives the game's entries; ours wraps its main phase and focus and
// runs its own panel phase. Values live in the DLL (netmenu.cpp): ExternalInterface "evnt" 990
// asks for them, 991 (row, step) changes one; the DLL answers through _root.lp2NetSet.
var lp2NetSel = false;
var lp2NetWasOver = false; // the mouse was over our entry last frame (select on entering it, like a roll over)
var lp2NetMain;
var lp2NetFocus;
var lp2NetAngle = -48; // the game's entries sit at 32, 16, 0, -16, -32
var lp2NetTexts = new Array("NETWORK","","","","","","","","");
var lp2NetRows = 4;
// Our texts are MsgOption messages 401-411 (the DLL adds them to every language of the message
// file); English when they are missing.
var lp2NetBase = 401;
var lp2NetEnglish = new Array("NETWORK","Share Custom Parts","Receive Custom Parts","Upload Speed","Auto","Custom parts sharing and upload speed for online play.","Let other players download and see your custom parts.","Download and show other players\' custom parts.","Upload speed to each player for custom parts and maps. Lower it on a slow connection.","Update Automatically","Download and install new versions of the mod when the game starts.");
function lp2NetMsg(i)
{
   var s = Message["get"]("MsgOption",lp2NetBase + i);
   if(s == undefined || s == "" || s.substr(0,5) == "DUMMY" || s.substr(0,7) == "Invalid" || s.substr(0,4) == "MSG_")
   {
      return lp2NetEnglish[i];
   }
   return s;
}
function lp2NetOnOff(on)
{
   var s = Message["get"]("MsgOption",91 + (on ? 1 : 0));
   return s == undefined || s == "" ? (on ? "On" : "Off") : s;
}
// The DLL's values: share, receive and update "0" / "1", the upload speed's text ("" = Auto). The texts are kept
// and put back every frame: the copied panel's text fields come back with their authored text when
// the rows change frames.
function lp2NetSet(share, receive, rate, update)
{
   lp2NetTexts = new Array(lp2NetMsg(0),lp2NetMsg(1),lp2NetOnOff(share == "1"),lp2NetMsg(2),lp2NetOnOff(receive == "1"),lp2NetMsg(3),rate == "" ? lp2NetMsg(4) : rate,lp2NetMsg(9),lp2NetOnOff(update == "1"));
   lp2NetApply(_root.mc_main);
   return true;
}
function lp2NetApply(m)
{
   var p = m.mc_subMenuNet;
   var t = lp2NetTexts;
   m.mc_mainMenu.mc_txtMenu6.txt_Menu.text = t[0];
   var i = 1;
   while(i <= lp2NetRows)
   {
      var r = p["mc_txtSubMenu" + i];
      if(r.mc_txtSubItem.txt_subItem.text != t[i * 2 - 1])
      {
         r.mc_txtSubItem.txt_subItem.text = t[i * 2 - 1];
      }
      if(r.txt_subMenu.text != t[i * 2])
      {
         r.txt_subMenu.text = t[i * 2];
      }
      i++;
   }
}
// The help line at the bottom: 0 the entry, 1-4 a row.
function lp2NetHint(n)
{
   _root.mc_CINESCO.cinescoMsg(lp2NetMsg(n == 4 ? 10 : 5 + n));
}
function lp2NetMouseOn(mc)
{
   return mc._visible && mc.hitTest(_root._xmouse,_root._ymouse,true);
}
function lp2NetSelect(m, i)
{
   lp2NetSel = i == 5;
   m.itemMainMenu.selectAndStop(i == 5 ? 4 : i);
   m.focusMainMenu();
   m.mc_SCOPE.playSCOPE();
   m.tweenEnd = i == 5 ? lp2NetAngle : m.mainRotationArray[i];
   m.cursorTween.continueTo(m.tweenEnd,m.cursorAnimeTime);
   IGame.sendEvent(EventNo.CURSOR_MOVE,i == 5 ? 4 : i);
   if(i == 5)
   {
      lp2NetHint(0);
   }
}
function lp2NetFocusAll()
{
   var m = _root.mc_main;
   lp2NetFocus.call(m);
   m.mc_mainMenu.mc_txtMenu6.gotoAndStop(1);
   if(lp2NetSel)
   {
      m.mc_mainMenu.mc_txtMenu5.gotoAndStop(1);
      m.mc_mainMenu.mc_txtMenu6.gotoAndStop(2);
   }   lp2NetApply(m);
}
function lp2NetMainPhase()
{
   var m = _root.mc_main;
   if(m.keyAccept == true && m.flagMainMenu == true)
   {
      var pad = m.pad;
      var up = pad.checkRepeat(Pad.Lup) || pad.checkRepeat(Pad.ALup) == true;
      var down = pad.checkRepeat(Pad.Ldown) || pad.checkRepeat(Pad.ALdown) == true;
      var over = lp2NetMouseOn(m.mc_mainMenu.mc_txtMenu6);
      var enter = over && !lp2NetWasOver;
      lp2NetWasOver = over;
      if(lp2NetSel)
      {
         if(up)
         {
            lp2NetSelect(m,4);
            return undefined;
         }
         if(down)
         {
            lp2NetSelect(m,0);
            return undefined;
         }
         if(pad.checkTrigger(Pad.Decide) == true || over && IGame.getMouseTrigger(IGame.MOUSE_BUTTON_L) == true)
         {
            lp2NetOpen(m);
            return undefined;
         }
         if(IGame.isMouseRollOver() == true && MouseId.MAIN_MENU0 <= IGame.getMouseSelectId() && IGame.getMouseSelectId() <= MouseId.MAIN_MENU4)
         {
            lp2NetSelect(m,IGame.getMouseSelectId() - MouseId.MAIN_MENU0);
            return undefined;
         }
      }
      else if(down && m.itemMainMenu.getIndex() == 4 || up && m.itemMainMenu.getIndex() == 0 || enter)
      {
         lp2NetSelect(m,5);
         return undefined;
      }
   }
   lp2NetMain.call(m);
}
function lp2NetOpen(m)
{
   IGame.sendEvent(EventNo.TRIGGER_DECIDE);
   m.flagMainMenu = false;
   m.mc_Disc.gotoAndStop("IN");
   var i = 0;
   while(i < 5)
   {
      IGame.setEnableMouse(MouseId.MAIN_MENU0 + i,false);
      i++;
   }
   m.mc_charData._visible = false;
   m.mc_subMenuNet._visible = true;
   m.lp2NetItem.selectAndStop(0);
   m.tweenEnd = m.subRotationArray1[0];
   m.cursorTween.continueTo(m.tweenEnd,m.cursorAnimeTime);
   m.onEnterFrame = lp2NetPanelPhase;
   lp2NetApply(m);
   lp2NetHint(1);
}
function lp2NetClose(m)
{
   m.mc_subMenuNet._visible = false;
   m.mc_Disc.gotoAndStop("OUT");
   m.mc_charData._visible = true;
   m.flagMainMenu = true;
   m.tweenEnd = lp2NetAngle;
   m.cursorTween.continueTo(m.tweenEnd,m.cursorAnimeTime);
   IGame.sendEvent(EventNo.CURSOR_MOVE,4);
   lp2NetHint(0);
   m.onEnterFrame = m.phaseMainMenu;
}
function lp2NetRow(m, i)
{
   m.lp2NetItem.selectAndStop(i);
   m.tweenEnd = m.subRotationArray1[i];
   m.cursorTween.continueTo(m.tweenEnd,m.cursorAnimeTime);
   IGame.sendEvent(EventNo.CURSOR_MOVE,4);
   lp2NetHint(i + 1);
}
function lp2NetStep(m, step)
{
   flash.external.ExternalInterface.call("evnt",991,m.lp2NetItem.getIndex(),step);
   IGame.sendEvent(EventNo.CURSOR_MOVE,4);
   lp2NetHint(m.lp2NetItem.getIndex() + 1);
}
function lp2NetPanelPhase()
{
   var m = _root.mc_main;
   if(m.keyAccept != true)
   {
      return undefined;
   }
   var pad = m.pad;
   var p = m.mc_subMenuNet;
   lp2NetApply(m);
   var index = m.lp2NetItem.getIndex();
   var click = IGame.getMouseTrigger(IGame.MOUSE_BUTTON_L) == true;
   if(pad.checkTrigger(Pad.Cancel) == true || IGame.getMouseTrigger(IGame.MOUSE_BUTTON_R) == true)
   {
      lp2NetClose(m);
   }
   else if(pad.checkRepeat(Pad.Lup) || pad.checkRepeat(Pad.ALup) == true)
   {
      lp2NetRow(m,(index + lp2NetRows - 1) % lp2NetRows);
   }
   else if(pad.checkRepeat(Pad.Ldown) || pad.checkRepeat(Pad.ALdown) == true)
   {
      lp2NetRow(m,(index + 1) % lp2NetRows);
   }
   else if(pad.checkRepeat(Pad.Lleft) == true || pad.checkRepeat(Pad.ALleft) == true || click && lp2NetMouseOn(p.mc_arrowL))
   {
      lp2NetStep(m,-1);
   }
   else if(pad.checkRepeat(Pad.Lright) == true || pad.checkRepeat(Pad.ALright) == true || pad.checkTrigger(Pad.Decide) == true || click && lp2NetMouseOn(p.mc_arrowR))
   {
      lp2NetStep(m,1);
   }
   else
   {
      var i = 0;
      while(i < lp2NetRows)
      {
         if(i != index && lp2NetMouseOn(p["mc_txtSubMenu" + (i + 1)]))
         {
            lp2NetRow(m,i);
            break;
         }
         i++;
      }
   }
}
function lp2NetInstall(m)
{
   var mm = m.mc_mainMenu;
   var t4 = mm.mc_txtMenu4;
   var t5 = mm.mc_txtMenu5;
   // AS depths of the movie's own objects are their file depth - 16384: file depth 4 is inside the ring's mask
   // (file depth 2 clips 3-27), the panel's file depth 213 above the Controls panel (212) and under the ring (283)
   t5.duplicateMovieClip("mc_txtMenu6",-16380);
   var t6 = mm.mc_txtMenu6;
   // the ring's mask is a 195 px circle at mc_mainMenu's origin: the same circle as its own mask too, in case
   // the timeline mask leaves dynamic clips out
   mm.createEmptyMovieClip("lp2NetMask",5);
   var k = mm.lp2NetMask;
   var rad = 195;
   k.beginFill(0,100);
   k.moveTo(rad,0);
   var a = 1;
   while(a <= 8)
   {
      var ang = a * Math.PI / 4;
      var mid = ang - Math.PI / 8;
      k.curveTo(Math.cos(mid) * rad / Math.cos(Math.PI / 8),Math.sin(mid) * rad / Math.cos(Math.PI / 8),Math.cos(ang) * rad,Math.sin(ang) * rad);
      a++;
   }
   k.endFill();
   t6.setMask(k);
   t6._x = t5._x + (t5._x - t4._x);
   t6._y = t5._y + (t5._y - t4._y);
   t6.gotoAndStop(1);
   m.mc_subMenu1.duplicateMovieClip("mc_subMenuNet",-16171);
   var p = m.mc_subMenuNet;
   p._visible = false;
   var i = lp2NetRows + 1;
   while(i <= 8)
   {
      p["mc_txtSubMenu" + i]._visible = false;
      i++;
   }
   m.lp2NetItem = new MenuItem(p,new Array("SUB1_1","SUB1_2","SUB1_3","SUB1_4"),true,0,3,true);
   lp2NetMain = m.phaseMainMenu;
   lp2NetFocus = m.focusMainMenu;
   m.phaseMainMenu = lp2NetMainPhase;
   m.focusMainMenu = lp2NetFocusAll;
   m.onEnterFrame = lp2NetMainPhase;
   lp2NetTexts = new Array(lp2NetMsg(0),lp2NetMsg(1),"",lp2NetMsg(2),"",lp2NetMsg(3),"",lp2NetMsg(9),""); // values come from the DLL
   t6.txt_Menu.text = lp2NetTexts[0];
   flash.external.ExternalInterface.call("evnt",990,0);
}
// The ring menu's script runs on mc_main's frame 15, after this one: install once it has started
// its main phase.
_root.createEmptyMovieClip("lp2NetWatch",16000);
_root.lp2NetWatch.onEnterFrame = function()
{
   var m = _root.mc_main;
   if(m.itemMainMenu != undefined && m.mc_mainMenu.mc_txtMenu5 != undefined && m.onEnterFrame == m.phaseMainMenu)
   {
      lp2NetInstall(m);
      this.removeMovieClip();
   }
};

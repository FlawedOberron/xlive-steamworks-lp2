// LP2 mods: a fifth Multiplayer entry, Bot Match (flash\online\id200), under the game's four. The
// game's code (uIdBattle) drives the selection and calls _root.selectItem; botmatch.cpp extends that
// to index 4, this script adds the clip. The mouse is ours, as the game only knows its four mouse
// ids: ExternalInterface "evnt" 992 selects the entry, 993 chooses it.
var lp2BotWasOver = false;
var lp2BotWasSel = false;
var lp2BotText = "BOT MATCH";
var lp2BotHint = "Play a match against bots, offline. Set up the rules, the bots and the map.";
function lp2BotInstall(m)
{
   var mm = m.mc_mainMenu;
   // file depth 24: a free depth inside the ring's mask (file depth 2 clips 3-27)
   mm.mc_txtMenu4.duplicateMovieClip("mc_txtMenu5",-16360);
   // five rows, spaced as the game's (50 px, 16 degrees), centred like its Options ring
   var i = 1;
   while(i <= 5)
   {
      mm["mc_txtMenu" + i]._y = (i - 3) * 50;
      m.mainRotationArray[i - 1] = (i - 3) * 16;
      i++;
   }
   var t = mm.mc_txtMenu5;
   t._x = mm.mc_txtMenu4._x;
   t.gotoAndStop("unfocus");
   m.menuMain.itemArray.push(t);
   var s = Message["get"]("MsgOnline",0);
   if(s != undefined && s != s.toUpperCase())
   {
      lp2BotText = "Bot Match";
   }
   t.txt_Menu.text = lp2BotText;
   m.tweenEnd = m.mainRotationArray[m.oldIndex];
   m.cursorTween.continueTo(m.tweenEnd,m.cursorAnimeTime);
   flash.external.ExternalInterface.call("evnt",992,1);
}
function lp2BotTick(m)
{
   var t = m.mc_mainMenu.mc_txtMenu5;
   // the row's frames bring their authored text back
   if(t.txt_Menu.text != lp2BotText)
   {
      t.txt_Menu.text = lp2BotText;
   }
   var over = t._visible && t.hitTest(_root._xmouse,_root._ymouse,true);
   if(over && !lp2BotWasOver && m.cursorPos != 4)
   {
      flash.external.ExternalInterface.call("evnt",992,0);
      _root.selectItem(4);
   }
   lp2BotWasOver = over;
   if(over && IGame.getMouseTrigger(IGame.MOUSE_BUTTON_L) == true)
   {
      flash.external.ExternalInterface.call("evnt",993,0);
   }
   var sel = m.cursorPos == 4;
   if(sel && !lp2BotWasSel)
   {
      _root.mc_CINESCO.cinescoMsg(lp2BotHint);
   }
   lp2BotWasSel = sel;
}
// mc_main's script builds the menu on its own frames: install once that has happened.
_root.createEmptyMovieClip("lp2BotWatch",16001);
_root.lp2BotWatch.onEnterFrame = function()
{
   var m = _root.mc_main;
   if(m.mc_mainMenu.mc_txtMenu5 != undefined)
   {
      lp2BotTick(m);
   }
   else if(m.menuMain != undefined && m.mainRotationArray.length == 4 && m.mc_mainMenu.mc_txtMenu4 != undefined)
   {
      lp2BotInstall(m);
   }
};

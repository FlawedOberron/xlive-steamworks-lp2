// LP2 mods: three more AI Enemy Battle rows, cloned from Game Type: Survival and Respawns below
// it (items 2, 3) and Bots above Map (item 4). Runs after the game's own root script; the native
// side moves the cursor over the rows and fills the values (and which show) through _root.lp2BotRows.
var lp2Bot;
function lp2BotRows(label2, value2, label3, value3, show2, show3, label4, value4)
{
   lp2Bot.mc_member2._visible = show2 == "1";
   lp2Bot.mc_member3._visible = show3 == "1";
   lp2Bot.mc_member2.mc_txtSubItem.txt_subItem.text = label2;
   lp2Bot.mc_member2.txt_subMenu.text = value2;
   lp2Bot.mc_member3.mc_txtSubItem.txt_subItem.text = label3;
   lp2Bot.mc_member3.txt_subMenu.text = value3;
   lp2Bot.setLetter(lp2Bot.mc_member2.mc_txtSubItem.txt_subItem,2);
   lp2Bot.mc_member4.mc_txtSubItem.txt_subItem.text = label4;
   lp2Bot.mc_member4.txt_subMenu.text = value4;
   lp2Bot.setLetter(lp2Bot.mc_member3.mc_txtSubItem.txt_subItem,2);
   lp2Bot.setLetter(lp2Bot.mc_member4.mc_txtSubItem.txt_subItem,2);
   return lp2Bot.mc_member4 != undefined;
}
// The panel's rows are built after this frame's script on Scaleform: set up on the first frame they exist.
function lp2Setup()
{
   lp2Bot = mc_main.mc_BOT;
   var lp2Gap = lp2Bot.mc_member1._y - lp2Bot.mc_member0._y;
   var lp2N = 2;
   while(lp2N <= 4)
   {
      var lp2Step = lp2N == 4 ? -2 : lp2N - 1; // Bots one gap above Map (item 0)
      lp2Bot.mc_member1.duplicateMovieClip("mc_member" + lp2N,900 + lp2N);
      lp2Bot.mc_arrowL2.duplicateMovieClip("mc_arrowL" + (lp2N + 1),910 + lp2N);
      lp2Bot.mc_arrowR2.duplicateMovieClip("mc_arrowR" + (lp2N + 1),920 + lp2N);
      lp2Bot["mc_member" + lp2N]._y = lp2Bot.mc_member1._y + lp2Gap * lp2Step;
      lp2Bot["mc_arrowL" + (lp2N + 1)]._y = lp2Bot.mc_arrowL2._y + lp2Gap * lp2Step;
      lp2Bot["mc_arrowR" + (lp2N + 1)]._y = lp2Bot.mc_arrowR2._y + lp2Gap * lp2Step;
      lp2Bot["mc_arrowL" + (lp2N + 1)]._visible = false;
      lp2Bot["mc_arrowR" + (lp2N + 1)]._visible = false;
      lp2Bot["mc_member" + lp2N].mc_txtSubItem.gotoAndStop(IGame.getLangShortName());
      lp2Bot["mc_member" + lp2N].gotoAndStop("unfocus");
      itemBotArray.push(lp2Bot["mc_member" + lp2N]);
      IGame.setMouseAction(lp2Bot["mc_member" + lp2N],MouseId.BOT_MENU1 + lp2N);
      IGame.setMouseAction(lp2Bot["mc_arrowL" + (lp2N + 1)],GlobalMouseId.ARROW_L);
      IGame.setMouseAction(lp2Bot["mc_arrowR" + (lp2N + 1)],GlobalMouseId.ARROW_R);
      lp2N++;
   }
   mc_main.botRotationArray.push(216,240,144);
}
_root.createEmptyMovieClip("lp2Wait",9990).onEnterFrame = function()
{
   if(mc_main.mc_BOT.mc_member1._y != undefined)
   {
      lp2Setup();
      delete this.onEnterFrame;
   }
};
var lp2SelectItem = selectItem;
selectItem = function(index)
{
   lp2SelectItem(index);
   if(phaseNo == PHASE_BOT)
   {
      var _loc1_ = 1;
      while(_loc1_ <= 5)
      {
         lp2Bot["mc_arrowL" + _loc1_]._visible = index == _loc1_ - 1;
         lp2Bot["mc_arrowR" + _loc1_]._visible = index == _loc1_ - 1;
         _loc1_++;
      }
   }
};

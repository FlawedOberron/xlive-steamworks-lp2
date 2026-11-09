// LP2 mods: the game setup screen (rules.cpp Setup). The DLL shows one page at a time in the screen's ten rows and steps values
// through the game's own code. lp2SetupRows lists the rows: items are "|" separated, the same for every category as the DLL has
// filtered them. An item of 520 or more is a row of ours: its label and value come from labels and values, not the game's texts.
var lp2SetupFirst = 520;
var lp2SetupItems = new Array();
var lp2SetupLabels = new Array();
var lp2SetupValues = new Array();
var lp2SetupKinds = new Array();
var lp2SetupGameUpdate = updateMember;
var lp2SetupI = 0;
while(lp2SetupI < 10)
{
   OnlineRule.offsetArray[lp2SetupFirst + lp2SetupI] = new Array(-1,0);
   lp2SetupI++;
}
// The slot list goes: it would stay drawn over the circle. The map and rule pictures show from the start.
// A cool tint over the grey backdrop, under the menu (mc_main sits at depth 1).
var lp2SetupTinted = false;
function lp2SetupLook()
{
   var menu = mc_main.mc_mainMenu;
   var n = 1;
   while(n <= 4)
   {
      menu["mc_txtMenu" + n]._visible = false;
      n++;
   }
   menu.mc_monitor._visible = true;
   menu.mc_rule._visible = true;
   lp2SetupCool(_root.mc_title);
   lp2SetupCool(menu.mc_rule);
   lp2SetupCool(mc_main.mc_Create1);
   lp2SetupCool(mc_main.mc_Create2);
   lp2SetupCool(mc_main.mc_Create3);
   lp2SetupCool(mc_main.mc_Cursor);
   lp2SetupCool(mc_main.mc_SCOPE);
   if(!lp2SetupTinted)
   {
      lp2SetupTinted = true;
      var tint = _root.createEmptyMovieClip("lp2Tint",0);
      tint.beginFill(801856,55);
      tint.moveTo(0,0);
      tint.lineTo(1280,0);
      tint.lineTo(1280,720);
      tint.lineTo(0,720);
      tint.lineTo(0,0);
      tint.endFill();
   }
}
// The campaign's feel: green accents to cyan, white text to pale cyan.
function lp2SetupCool(mc)
{
   if(mc != undefined)
   {
      new Color(mc).setTransform({ra:55,rb:0,ga:95,gb:0,ba:105,bb:55,aa:100,ab:0});
   }
}
// Layout per row: the game's own, so values sit centred between where the arrows go. A row with no value gets the whole width for
// its label, and a sub-menu's ">" goes to the far right.
function lp2SetupFit(row, value)
{
   var title = row.txt_TitleItem;
   var field = row.txt_CreateItem;
   if(row.lp2Title == undefined)
   {
      row.lp2Title = title._width;
      row.lp2X = field._x;
      row.lp2Width = field._width;
      row.lp2Format = field.getTextFormat();
      row.lp2Right = row.mc_arrowR._x + row.mc_arrowR._width;
      if(row.lp2Right < field._x + field._width)
      {
         row.lp2Right = field._x + field._width;
      }
   }
   title._width = row.lp2Title;
   field._x = row.lp2X;
   field._width = row.lp2Width;
   field.setNewTextFormat(row.lp2Format);
   field.setTextFormat(row.lp2Format);
   if(value == "")
   {
      title._width = row.lp2Right - title._x;
   }
   else if(value == ">")
   {
      field._x = row.lp2Right - 74;
      field._width = 60;
      title._width = field._x - title._x;
      var far = field.getTextFormat();
      far.align = "right";
      field.setTextFormat(far);
   }
   else if(field.textWidth > row.lp2Width - 8)
   {
      // a long value (a game mode, a profile name): the field grows left into the label's unused room (the arrows are the game's: it moves them per row)
      var right = row.lp2X + row.lp2Width;
      var left = title._x + Math.min(title.textWidth + 8,row.lp2Title) + 16;
      if(left < right - field.textWidth - 12)
      {
         left = right - field.textWidth - 12;
      }
      field._x = left;
      field._width = right - left;
      title._width = left - title._x;
   }
}
function lp2SetupText()
{
   lp2SetupLook();
   var i = 0;
   while(i < lp2SetupItems.length)
   {
      var row = itemMainArray[i];
      if(lp2SetupItems[i] >= lp2SetupFirst)
      {
         var kind = Number(lp2SetupKinds[i]);
         var selected = i == cursorPos;
         row.txt_TitleItem.text = lp2SetupLabels[i];
         row.txt_CreateItem.text = lp2SetupValues[i];
         row.mc_arrowL._visible = selected && (kind & 2) != 0;
         row.mc_arrowR._visible = selected && (kind & 4) != 0;
         lp2SetupFit(row,lp2SetupValues[i]);
         if(selected)
         {
            IGame.setEnableMouse(GlobalMouseId.ARROW_L,(kind & 2) != 0);
            IGame.setEnableMouse(GlobalMouseId.ARROW_R,(kind & 4) != 0);
         }
      }
      else
      {
         if(lp2SetupLabels[i] != undefined && lp2SetupLabels[i] != "")
         {
            row.txt_TitleItem.text = lp2SetupLabels[i];
         }
         lp2SetupFit(row,"-");
      }
      i++;
   }
}
var lp2SetupGameSelect = selectItem;
selectItem = function(index)
{
   lp2SetupGameSelect(index);
   lp2SetupText();
};
updateMember = function()
{
   lp2SetupGameUpdate();
   lp2SetupText();
};
function lp2SetupRows(items, labels, values, kinds)
{
   var list = items.split("|");
   var i = 0;
   while(i < list.length)
   {
      list[i] = Number(list[i]);
      i++;
   }
   lp2SetupItems = list;
   lp2SetupLabels = labels.split("|");
   lp2SetupValues = values.split("|");
   lp2SetupKinds = kinds.split("|");
   var c = 0;
   while(c < OnlineRule.mainItemArray.length)
   {
      OnlineRule.mainItemArray[c] = list;
      c++;
   }
   updateMember();
   return true;
}
function lp2SetupTitle(title)
{
   var n = 1;
   while(n <= 3)
   {
      var tab = mc_main["mc_Create" + n].mc_TAB;
      tab.txt_tab1.text = title;
      tab.txt_tab2.text = "";
      tab.txt_tab3.text = "";
      lp2SetupOneTab(mc_main["mc_Create" + n],tab);
      n++;
   }
   mc_main.mc_LB._visible = false;
   mc_main.mc_RB._visible = false;
   return true;
}
// Only the title's tab: the other two are unnamed shapes, so a mask cuts them away and keeps the rule running under them. In
// mc_TAB's coordinates the first tab ends at x 565 and the rule starts at y 0; the slant follows the second tab's edge.
function lp2SetupOneTab(parent, tab)
{
   if(parent == undefined || tab == undefined)
   {
      return undefined;
   }
   var k = parent.lp2TabMask;
   if(k == undefined)
   {
      k = parent.createEmptyMovieClip("lp2TabMask",20000);
      k.beginFill(16777215,100);
      k.moveTo(-40,-50);
      k.lineTo(570,-50);
      k.lineTo(570,-5);
      k.lineTo(565,0);
      k.lineTo(1500,0);
      k.lineTo(1500,70);
      k.lineTo(-40,70);
      k.lineTo(-40,-50);
      k.endFill();
      tab.setMask(k);
   }
   k._x = tab._x;
   k._y = tab._y;
   k._xscale = tab._xscale;
   k._yscale = tab._yscale;
}

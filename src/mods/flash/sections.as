// LP2 mods: the create-match screen's tabs become Game + sliding sub-menus (rules.cpp Sections). Tab 1
// is always Game; tabs 2 and 3 show two sub-menus and slide over the rest. The native side sends each
// tab's rows as "cat:page.index,page.index;cat:..." naming rows of the arrays as they were at the first
// call (the game's plus ours), and edits its own row table the same way.
var lp2Base;
function lp2SectionRows(tab, list)
{
   if(lp2Base == undefined)
   {
      lp2Base = new Array(OnlineRule.mainItemArray.slice(),OnlineRule.sub1ItemArray.slice(),OnlineRule.sub2ItemArray.slice());
   }
   var target = tab == "1" ? OnlineRule.mainItemArray : (tab == "2" ? OnlineRule.sub1ItemArray : OnlineRule.sub2ItemArray);
   var cats = list.split(";");
   var i = 0;
   while(i < cats.length)
   {
      var kv = cats[i].split(":");
      var rows = new Array();
      var items = kv[1].split(",");
      var j = 0;
      while(j < items.length)
      {
         var at = items[j].split(".");
         rows.push(lp2Base[Number(at[0])][Number(kv[0])][Number(at[1])]);
         j++;
      }
      // Settings 3's last row is hidden on PC (the game's cross-play row): keep one there
      if(tab == "3")
      {
         rows.push(OnlineRule.ITEM_PCCROSS);
      }
      target[Number(kv[0])] = rows;
      i++;
   }
   return true;
}
function lp2SectionTabs(a, b, c)
{
   var n = 1;
   while(n <= 3)
   {
      var t = mc_main["mc_Create" + n].mc_TAB;
      t.txt_tab1.text = a;
      t.txt_tab2.text = b;
      t.txt_tab3.text = c;
      n++;
   }
   updateMember();
   return true;
}

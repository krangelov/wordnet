gfwordnet = {}

gfwordnet.grammar_url = "http://cloud.grammaticalframework.org/robust/Parse.pgf"

gfwordnet.grammar_call=function(querystring,cont,errcont) {
    http_get_json(gfwordnet.grammar_url+querystring,cont,errcont)
}

gfwordnet.sense_url = "http://www.grammaticalframework.org/~krasimir/SenseService.fcgi"

gfwordnet.sense_call=function(querystring,cont,errcont) {
    http_get_json(gfwordnet.sense_url+querystring,cont,errcont)
}

gfwordnet.initialize = function () {
}

gfwordnet.search = function (from, input, result) {
	function errcont(text,code) { }
	function extract_linearization(lins) {
		var indices = {"ParseBul": 1, "ParseEng": 2, "ParseSwe": 3};
		for (var i in lins) {
			var lin = lins[i];
			this[indices[lin.to]].appendChild(text(lin.text));
		}
	}
	function extract_senses(senses) {
		var index = 1;
		for (var i in senses) {
			result.appendChild(tr(node("td",{colspan: 4},[text(index+". "+senses[i].gloss)]))); index++;
			for (var lex_id in senses[i].lex_ids) {
				var icon;
				var row = this[lex_id];
				if (senses[i].lex_ids[lex_id].indexOf("unchecked") >= 0)
					icon = img("unchecked.png");
				else
					icon = img("checked.png");
				row[0].insertBefore(icon, row[0].firstChild);
				result.appendChild(tr(row));
			}
		}
	}
	function extract_search(lemmas) {
		function cell(contents) {
			return node("td",{onclick: "gfwordnet.onclick_cell(this)"},contents);
		}

		clear(result);
		var rows        = {};
		var lexical_ids = ""
		result.appendChild(tr([th(text("Abstract")),th(text("Bulgarian")),th(text("English")),th(text("Swedish"))]));
		for (var i in lemmas) {
			var lemma = lemmas[i].lemma;
			if (!(lemma in rows)) {
				var row = [cell([text(lemma)]),cell([]),cell([]),cell([])];
				rows[lemma] = row;
				lexical_ids = lexical_ids+" "+lemma;

				gfwordnet.grammar_call("?command=c-linearize&to=ParseBul%20ParseEng%20ParseSwe&tree="+encodeURIComponent(lemma),bind(extract_linearization,row),errcont);
			}
		}
		gfwordnet.sense_call("?lexical_ids="+encodeURIComponent(lexical_ids),bind(extract_senses,rows),errcont);
    }

	gfwordnet.grammar_call("?command=c-lookupmorpho&input="+encodeURIComponent(input)+"&from="+from,extract_search,errcont);
}

gfwordnet.onclick_cell = function (cell) {
	if (cell.parentNode.nextSibling != null && 
	    cell.parentNode.nextSibling.firstChild != null && 
	    cell.parentNode.nextSibling.firstChild.className != "details") {
		var row = tr(node("td",{colspan: 4, class: "details"},[text("test")]));
		cell.parentNode.parentNode.insertBefore(row, cell.parentNode.nextSibling);
	}

	var siblings = cell.parentNode.children;
	for (var i = 0; i < siblings.length; i++) {
		siblings[i].className = "unselected"
	}
	cell.className = "selected";
}

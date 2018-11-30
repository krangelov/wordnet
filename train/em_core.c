#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <gu/string.h>
#include <gu/mem.h>
#include <gu/seq.h>
#include <gu/map.h>
#include <gu/string.h>
#include <gu/ucs.h>
#include <gu/utf8.h>
#include <pgf/pgf.h>
#include "em_core.h"

// #define DEBUG

struct EMState {
	GuPool* pool;
	PgfPGF *pgf;
	GuBuf* dtrees;
	GuBuf* fields;
	GuMap* stats;
	GuBuf* root_choices;
	prob_t bigram_total;
	prob_t unigram_total;
	prob_t bigram_smoothing;
	prob_t unigram_smoothing;
	GuBuf* pcs;
	bool break_trees;
	GuMap* callbacks;
};

#ifdef DEBUG
static void
print_tree(DepTree* dtree)
{
	printf("([");
	size_t n_choices = gu_buf_length(dtree->choices);
	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* choice =
			gu_buf_index(dtree->choices, SenseChoice, i);

		if (i > 0) printf(" ");
		printf("%e", choice->prob);
	}
	printf("]");
	for (size_t i = 0; i < dtree->n_children; i++) {
		putc(' ', stdout);
		print_tree(dtree->child[i]);
	}
	putc(')', stdout);
}
#endif

EMState*
em_new_state(PgfPGF* pgf)
{
	GuPool* pool = gu_new_pool();

	EMState* state = gu_new(EMState, pool);
	state->pool = pool;
	state->dtrees = gu_new_buf(DepTree*, pool);
	state->fields = NULL;
	state->stats  = gu_new_string_map(FunStats*, NULL, pool);
	state->bigram_total = 0;
	state->unigram_total = 0;
	state->bigram_smoothing = INFINITY;
	state->unigram_smoothing = INFINITY;
	state->pcs = gu_new_buf(ProbCount*, pool);

	state->root_choices = gu_new_buf(SenseChoice, pool);
	state->break_trees = 0;
	state->callbacks = gu_new_string_map(EMRankingCallback, &gu_null_struct, pool);
	state->pgf = pgf;

	return state;
}

void
em_free_state(EMState* state)
{
	gu_pool_free(state->pool);
}



#define CONLL_NUM_FIELDS 10
typedef GuString CONLLFields[CONLL_NUM_FIELDS];

typedef struct {
	PgfMorphoCallback base;
	EMState* state;
	GuBuf* parent_choices;
	GuBuf* choices;
} LookupCallback;

typedef prob_t (*Oper)(prob_t x, prob_t y);

static prob_t
log_add(prob_t x, prob_t y)
{
	if (x == INFINITY)
		return y;
	if (y == INFINITY)
		return x;
	if (x < y)
		return x - log1p(exp(x - y));
	else
		return y - log1p(exp(y - x));
}

static prob_t
log_max(prob_t x, prob_t y)
{
	return ((x < y) ? x : y);
}

void
em_setup_preserve_trees(EMState *state)
{
	state->fields = gu_new_buf(GuBuf*, state->pool);
}

typedef struct {
	GuMapItor clo;
	EMState *state;
} SmoothingItor;

static void
smoothing_iter(GuMapItor* clo, const void* key, void* value, GuExn* err)
{
	SmoothingItor *self = gu_container(clo, SmoothingItor, clo);

	PgfCId fun = (PgfCId) key;

	FunStats **stats =
		gu_map_insert(self->state->stats, fun);
	if (*stats == NULL) {
		*stats = gu_new(FunStats, self->state->pool);
		(*stats)->fun = fun;
		(*stats)->pc.prob  = 0;
		(*stats)->pc.count = self->state->unigram_smoothing;
		(*stats)->mods =
			gu_new_string_map(ProbCount*, NULL, self->state->pool);
		gu_buf_push(self->state->pcs, ProbCount*, &(*stats)->pc);
	}
	self->state->unigram_total += exp(-self->state->unigram_smoothing);
}

void
em_setup_bigram_smoothing(EMState *state, prob_t prob)
{
	state->bigram_smoothing = -log(prob);
}

void
em_setup_unigram_smoothing(EMState *state, prob_t count)
{
	state->unigram_smoothing = -log(count);

	SmoothingItor itor;
	itor.clo.fn  = smoothing_iter;
	itor.state = state;
	pgf_iter_functions(state->pgf, &itor.clo, NULL);
}

static void
lookup_callback(PgfMorphoCallback* callback,
	            PgfCId lemma, GuString analysis, prob_t prob,
	            GuExn* err)
{
	LookupCallback* self =
		gu_container(callback, LookupCallback, base);

	bool found = false;
	for (int i = 0; i < gu_buf_length(self->choices); i++) {
		SenseChoice* choice = gu_buf_index(self->choices, SenseChoice, i);
		if (strcmp(choice->stats->fun,lemma) == 0) {
			found = true;
			break;
		}
	}

	if (!found) {
		SenseChoice* choice = gu_buf_extend(self->choices);
		choice->prob = 0;
		choice->prob_counts = NULL;

		FunStats** stats =
			gu_map_insert(self->state->stats, lemma);
		if (*stats == NULL) {
			*stats = gu_new(FunStats, self->state->pool);
			(*stats)->fun = lemma;
			(*stats)->pc.prob  = 0;
			(*stats)->pc.count = INFINITY;
			(*stats)->mods =
				gu_new_string_map(ProbCount*, NULL, self->state->pool);
			gu_buf_push(self->state->pcs, ProbCount*, &(*stats)->pc);
		}
		choice->stats = *stats;
	}
}

static prob_t
get_pgf_prob(EMState* state, PgfCId fun)
{
	PgfType* ty =
		pgf_function_type(state->pgf, fun);
	if (ty == NULL) {
		printf("Unknown function %s\n", fun);
		exit(1);
	}

	return pgf_category_prob(state->pgf, ty->cid) +
	       pgf_function_prob(state->pgf, fun);
}

static void
init_counts(EMState* state, DepTree* dtree, GuBuf* parent_choices)
{
	size_t n_choices = gu_buf_length(dtree->choices);
	size_t n_parent_choices = gu_buf_length(parent_choices);

	prob_t p1 = log(n_choices);
	prob_t p2 = p1 + log(n_parent_choices);

	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* choice =
			gu_buf_index(dtree->choices, SenseChoice, i);

		choice->prob = 0;
		choice->stats->pc.count =
			log_add(choice->stats->pc.count,p1);

		choice->prob_counts =
			gu_new_n(ProbCount*, n_parent_choices, state->pool);

		for (int j = 0; j < n_parent_choices; j++) {
			SenseChoice* parent_choice =
				gu_buf_index(parent_choices, SenseChoice, j);

			ProbCount** pc =
				gu_map_insert(parent_choice->stats->mods, choice->stats->fun);
			if (*pc == NULL) {
				prob_t back_off =
					get_pgf_prob(state,parent_choice->stats->fun) +
					get_pgf_prob(state,choice->stats->fun);

				*pc = gu_new(ProbCount, state->pool);
				(*pc)->prob  = state->bigram_smoothing + back_off;
				(*pc)->count = INFINITY;
				gu_buf_push(state->pcs, ProbCount*, *pc);
			}

			choice->prob_counts[j] = *pc;

			choice->prob_counts[j]->count =
				log_add(choice->prob_counts[j]->count, p2);
		}
	}
}

static void
filter_dep_tree(EMState* state, DepTree* dtree, GuBuf* buf, GuBuf* parent_choices)
{
	CONLLFields* fields = gu_buf_index(buf, CONLLFields, dtree->index);
	size_t n_choices = gu_buf_length(dtree->choices);

	int max[2] = {INT_MIN, INT_MAX};
	int stats[n_choices][2];
	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* choice =
			gu_buf_index(dtree->choices, SenseChoice, i);

		PgfType* ty =
			pgf_function_type(state->pgf, choice->stats->fun);

		EMRankingCallback callback =
			gu_map_get(state->callbacks, ty->cid, EMRankingCallback);
		if (callback != NULL) {
			callback(choice, buf, dtree, stats[i]);
		} else {
			stats[i][0] = 0;
			stats[i][1] = 0;
		}
		if (stats[i][0] > max[0]) {
			max[0] = stats[i][0];
			max[1] = stats[i][1];
		} else if (stats[i][0] == max[0]) {
			if (stats[i][1] < max[1])
				max[1] = stats[i][1];
		}
	}

	size_t index = 0;
	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* choice =
			gu_buf_index(dtree->choices, SenseChoice, i);
		if (stats[i][0] == max[0] && stats[i][1] == max[1]) {
			SenseChoice* dest =
				gu_buf_index(dtree->choices, SenseChoice, index);
			*dest = *choice;
			index++;
		}
	}

	gu_buf_trim_n(dtree->choices, n_choices - index);

	init_counts(state, dtree, parent_choices);

	for (size_t i = 0; i < dtree->n_children; i++) {
		filter_dep_tree(state, dtree->child[i], buf, dtree->choices);
	}
}

static DepTree*
build_dep_tree(EMState* state, PgfConcr* concr,
               GuBuf* buf, size_t index, CONLLFields* fields,
               GuBuf* parent_choices)
{
	GuString id = (*fields)[0];

	// first count how many children we have
	size_t n_children = 0;
	for (int i = 0; i < gu_buf_length(buf); i++) {
		CONLLFields* fields = gu_buf_index(buf, CONLLFields, i);
		if (strcmp((*fields)[6],id) == 0) {
			n_children++;
		}
	}

	DepTree* dtree = gu_new_flex(state->pool, DepTree, child, n_children);
	dtree->index      = index;
	dtree->choices    = gu_new_buf(SenseChoice, state->pool);
	dtree->n_children = n_children;

	LookupCallback callback;
	callback.base.callback = lookup_callback;
	callback.state = state;
	callback.parent_choices = parent_choices;
	callback.choices = dtree->choices;

	pgf_lookup_morpho(concr, (*fields)[1], &callback.base, NULL);
	if (gu_buf_length(dtree->choices) == 0) {
		// try with lower case
		char buffer[strlen((*fields)[1])*6+1];

		const uint8_t* src = (uint8_t*) (*fields)[1];
		uint8_t* dst = (uint8_t*) buffer;

		while (*src) {
			GuUCS ucs = gu_utf8_decode(&src);
			ucs = gu_ucs_to_lower(ucs);
			gu_utf8_encode(ucs, &dst);
		}
		*(dst++) = 0;

		pgf_lookup_morpho(concr, buffer, &callback.base, NULL);
	}

	state->unigram_total++;

	// now build the children
	int pos = 0;
	for (int i = 0; i < gu_buf_length(buf); i++) {
		CONLLFields* fields = gu_buf_index(buf, CONLLFields, i);
		if (strcmp((*fields)[6],id) == 0) {
			dtree->child[pos++] = 
				build_dep_tree(state, concr,
							   buf, i, fields,
							   dtree->choices);
			state->bigram_total++;
		}
	}

	return dtree;
}

DepTree*
em_new_dep_tree(EMState* state, DepTree* parent,
                PgfCId fun, GuString lbl,
                size_t index, size_t n_children)
{
	DepTree* dtree = gu_new_flex(state->pool, DepTree, child, n_children);
	dtree->index      = index;
	dtree->choices    = gu_new_buf(SenseChoice, state->pool);
	dtree->n_children = n_children;

	SenseChoice* choice = gu_buf_extend(dtree->choices);
	choice->prob = 0;

	FunStats** stats =
		gu_map_insert(state->stats, fun);
	if (*stats == NULL) {
		*stats = gu_new(FunStats, state->pool);
		(*stats)->fun = fun;
		(*stats)->pc.prob  = 0;
		(*stats)->pc.count = INFINITY;
		(*stats)->mods =
			gu_new_string_map(ProbCount*, NULL, state->pool);
		gu_buf_push(state->pcs, ProbCount*, &(*stats)->pc);
	}
	choice->stats = *stats;

	choice->stats->pc.count =
		log_add(choice->stats->pc.count,0);

	choice->prob_counts =
		gu_new_n(ProbCount*, 1, state->pool);

	if (parent != NULL) {
		SenseChoice* parent_choice =
			gu_buf_index(parent->choices, SenseChoice, 0);

		ProbCount** pc =
			gu_map_insert(parent_choice->stats->mods, choice->stats->fun);
		if (*pc == NULL) {
			prob_t back_off =
				get_pgf_prob(state,parent_choice->stats->fun) +
				get_pgf_prob(state,choice->stats->fun);

			*pc = gu_new(ProbCount, state->pool);
			(*pc)->prob  = state->bigram_smoothing + back_off;
			(*pc)->count = INFINITY;
			gu_buf_push(state->pcs, ProbCount*, *pc);
		}

		choice->prob_counts[0] = *pc;

		choice->prob_counts[0]->count =
			log_add(choice->prob_counts[0]->count, 0);
	}

	state->unigram_total++;
	state->bigram_total += n_children;

	return dtree;
}

DepTree*
em_new_conll_dep_tree(EMState* state, GuString lang, GuSeq* fields)
{
	PgfConcr* concr = pgf_get_language(state->pgf, lang);
	if (concr == NULL)
		return NULL;

	GuBuf buf[1];
	buf->seq = fields;
	buf->avail_len = fields->len;

	DepTree *dtree = NULL;
	for (int i = 0; i < gu_buf_length(buf); i++) {
		CONLLFields* fields = gu_buf_index(buf, CONLLFields, i);
		if (strcmp((*fields)[6], "0") == 0) {
			dtree = build_dep_tree(state, concr,
								   buf, i, fields,
								   state->root_choices);
			filter_dep_tree(state, dtree, buf, state->root_choices);
			break;
		}
	}
	return dtree;
}

void
em_add_dep_tree(EMState* state, DepTree* dtree)
{
	gu_buf_push(state->dtrees, DepTree*, dtree);
}

int
em_import_treebank(EMState* state, GuString fpath, GuString lang)
{
	PgfConcr* concr = pgf_get_language(state->pgf, lang);
	if (concr == NULL) {
		fprintf(stderr, "Couldn't find language %s", lang);
		return 0;
	}

	FILE* inp;
	if (fpath == NULL || *fpath == 0)
		inp = stdin;
	else
		inp = fopen(fpath, "r");
	if (!inp) {
		fprintf(stderr, "Error opening %s\n", fpath);
		return 0;
	}

	GuPool* tmp_pool;
	if (state->fields == NULL)
		tmp_pool = gu_new_pool();
	else
		tmp_pool = state->pool;
	GuBuf* buf = gu_new_buf(CONLLFields, tmp_pool);

	char line[2048];
	while (fgets(line, sizeof(line), inp)) {
		int len = strlen(line);
		if (len < 1 || line[len-1] != '\n') {
			fprintf(stderr, "Error in reading. Last read: %s\n", line);
			if (state->fields == NULL) {
				gu_pool_free(tmp_pool);
			}
			fclose(inp);
			return 0;
		}

		// skip comments
		if (line[0] == '#')
			continue;

		// empty line signals the end of a sentence
		if (line[0] == '\n') {
			DepTree *dtree = NULL;
			for (int i = 0; i < gu_buf_length(buf); i++) {
				CONLLFields* fields = gu_buf_index(buf, CONLLFields, i);
				if (strcmp((*fields)[6], "0") == 0) {
					dtree = build_dep_tree(state, concr,
					                       buf, i, fields,
					                       state->root_choices);
					filter_dep_tree(state, dtree, buf, state->root_choices);
					gu_buf_push(state->dtrees, DepTree*, dtree);
					break;
				}
			}

			if (state->fields == NULL) {
				// if we don't need to save the CoNLL fields,
				// release the current pool and create a new one
				gu_pool_free(tmp_pool);
				tmp_pool = gu_new_pool();
			} else {
				// add the fields to the state
				gu_buf_push(state->fields, GuBuf*, buf);
			}
			buf = gu_new_buf(CONLLFields, tmp_pool);
			continue;
		}

		size_t n_fields = 0;
		CONLLFields* fields = gu_buf_extend(buf);
		char* start = line;
		char* end   = line;
		for (;;) {
			while (*end != '\n' && *end != '\t') {
				end++;
			}

			size_t len = end-start;
			char* field = gu_malloc(tmp_pool, len+1);
			memcpy(field, start, len);
			field[len] = 0;

			if (n_fields >= CONLL_NUM_FIELDS) {
				fprintf(stderr, "Too many fields in: %s\n", line);
				if (state->fields == NULL) {
					gu_pool_free(tmp_pool);
				}
				fclose(inp);
				return 0;
			}

			(*fields)[n_fields++] = field;

			if (*end == '\n')
				break;

			end++;
			start = end;
		}

		while (n_fields < CONLL_NUM_FIELDS) {
			(*fields)[n_fields++] = "";
		}
    }

	if (state->fields == NULL) {
		gu_pool_free(tmp_pool);
	}

	fclose(inp);
	return 1;
}

static void
function_iter(GuMapItor* clo, const void* key, void* value, GuExn* err)
{
	SmoothingItor *self = gu_container(clo, SmoothingItor, clo);

	PgfCId fun = (PgfCId) key;

	FunStats **stats =
		gu_map_insert(self->state->stats, fun);
	if (*stats == NULL) {
		prob_t prob =
			pgf_category_prob(self->state->pgf,
			                  pgf_function_type(self->state->pgf, fun)->cid) +
			pgf_function_prob(self->state->pgf, fun);

		*stats = gu_new(FunStats, self->state->pool);
		(*stats)->fun = fun;
		(*stats)->pc.prob  = prob;
		(*stats)->pc.count = INFINITY;
		(*stats)->mods =
			gu_new_string_map(ProbCount*, NULL, self->state->pool);
		gu_buf_push(self->state->pcs, ProbCount*, &(*stats)->pc);
	}
}

int
em_load_model(EMState* state, GuString fpath)
{
	FILE* inp = fopen(fpath, "r");
	if (!inp) {
		fprintf(stderr, "Error opening %s\n", fpath);
		return 0;
	}

	SmoothingItor itor;
	itor.clo.fn = function_iter;
	itor.state  = state;
	pgf_iter_functions(state->pgf, &itor.clo, NULL);

	char line[2048];
	while (fgets(line, sizeof(line), inp)) {
		int len = strlen(line);
		if (len < 1 || line[len-1] != '\n') {
			fprintf(stderr, "Error in reading. Last read: %s\n", line);
			fclose(inp);
			return 0;
		}

		size_t n_fields = 0;
		GuString fields[3];
		char* start = line;
		char* end   = line;
		for (;;) {
			while (*end != '\n' && *end != '\t') {
				end++;
			}

			if (n_fields >= 3) {
				fprintf(stderr, "Too many fields in: %s\n", line);
				fclose(inp);
				return 0;
			}

			char c = *end;
			*end = 0;
			fields[n_fields++] = start;

			if (c == '\n')
				break;

			end++;
			start = end;
		}

		if (n_fields != 3) {
			fprintf(stderr, "Too few fields in: %s\n", line);
			fclose(inp);
			return 0;
		}

		FunStats* stats = gu_map_get(state->stats, fields[0], FunStats*);
		fields[1] = *((GuString*) gu_map_find_key(state->stats, fields[1]));
		ProbCount** pc = gu_map_insert(stats->mods, fields[1]);
		if (*pc == NULL) {
			prob_t back_off =
				get_pgf_prob(state,fields[0]) +
				get_pgf_prob(state,fields[1]);

			prob_t bigram_smoothing1m = 
				-log1p(-exp(-state->bigram_smoothing));

			prob_t prob =
				log_add(state->bigram_smoothing + back_off,
                        bigram_smoothing1m + atof(fields[2]));

			*pc = gu_new(ProbCount, state->pool);
			(*pc)->prob  = prob;
			(*pc)->count = INFINITY;
			gu_buf_push(state->pcs, ProbCount*, *pc);
		}
	}

	fclose(inp);
	return 1;
}

int
em_unigram_count(EMState* state)
{
	return state->unigram_total;
}

int
em_bigram_count(EMState* state)
{
	return state->bigram_total;
}

void
em_set_ranking_callback(EMState* state,
                        PgfCId cat,
                        EMRankingCallback *ranking_callback)
{
	cat = gu_string_copy(cat, state->pool);
	gu_map_put(state->callbacks, cat, EMRankingCallback*, ranking_callback);
}

int
dtree_match_label(GuBuf* buf, DepTree *dtree, GuString lbl)
{
	CONLLFields* fields =
		gu_buf_index(buf, CONLLFields, dtree->index);
			
	if (strcmp((*fields)[7], lbl) == 0)
		return 1;

	return 0;
}

int
dtree_match_pos(GuBuf* buf, DepTree *dtree, GuString pos)
{
	CONLLFields* fields =
		gu_buf_index(buf, CONLLFields, dtree->index);
			
	if (strcmp((*fields)[3], pos) == 0)
		return 1;

	return 0;
}

int
dtree_match_same_choice(SenseChoice *choice, DepTree *dtree)
{
	size_t n_choices = gu_buf_length(dtree->choices);
	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* mod_choice =
			gu_buf_index(dtree->choices, SenseChoice, i);
			if (choice->stats == mod_choice->stats)
				return 1;
	}
	return 0;
}

static prob_t
tree_sum_estimation(DepTree* dtree, Oper oper)
{
	size_t n_choices = gu_buf_length(dtree->choices);
	if (n_choices == 0) {
		prob_t prob = 0;
		for (size_t i = 0; i < dtree->n_children; i++) {
			prob += tree_sum_estimation(dtree->child[i], oper);
		}
		return prob;
	} else {
		prob_t prob = INFINITY;
		for (size_t i = 0; i < n_choices; i++) {
			SenseChoice* choice =
				gu_buf_index(dtree->choices, SenseChoice, i);
			prob = oper(prob, choice->prob);
		}
		return prob;
	}
}

static prob_t
tree_edge_estimation(size_t head_i, DepTree* mod, Oper oper)
{
	prob_t edge_prob = INFINITY;

	size_t n_choices = gu_buf_length(mod->choices);
	if (n_choices == 0) {
		edge_prob = tree_sum_estimation(mod, oper);
		if (edge_prob == INFINITY)
			return 0;
	}

	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* mod_choice =
			gu_buf_index(mod->choices, SenseChoice, i);
		edge_prob =
		   oper(edge_prob,
				mod_choice->prob_counts[head_i]->prob +
				mod_choice->prob);
	}
	return edge_prob;
}

static void
tree_estimation(DepTree* dtree, Oper oper)
{
	for (size_t i = 0; i < dtree->n_children; i++) {
		tree_estimation(dtree->child[i], oper);
	}

	size_t n_choices = gu_buf_length(dtree->choices);
	for (size_t i = 0; i < n_choices; i++) {
		SenseChoice* choice =
			gu_buf_index(dtree->choices, SenseChoice, i);

		prob_t prob = 0;
		for (size_t j = 0; j < dtree->n_children; j++) {
			prob += tree_edge_estimation(i, dtree->child[j], oper);
		}

		choice->prob = prob;
	}
}

static void
tree_counting(EMState* state, DepTree* dtree, prob_t* outside_probs)
{
	size_t n_head_choices = gu_buf_length(dtree->choices);
	for (size_t j = 0; j < n_head_choices; j++) {
		SenseChoice* head_choice =
			gu_buf_index(dtree->choices, SenseChoice, j);

		prob_t prob = outside_probs[j] + head_choice->prob;
		head_choice->stats->pc.count = 
			log_add(head_choice->stats->pc.count, prob);
	}

	for (size_t i = 0; i < dtree->n_children; i++) {
		size_t n_child_choices =
			gu_buf_length(dtree->child[i]->choices);
		prob_t child_outside_probs[n_child_choices];

		if (n_head_choices > 0) {
			for (size_t k = 0; k < n_child_choices; k++) {
				child_outside_probs[k] = INFINITY;
			}

			for (size_t j = 0; j < n_head_choices; j++) {
				SenseChoice* head_choice =
					gu_buf_index(dtree->choices, SenseChoice, j);

				if (head_choice->prob < INFINITY) {
					prob_t prob =
						outside_probs[j] + head_choice->prob -
						tree_edge_estimation(j, dtree->child[i], log_add);

					for (size_t k = 0; k < n_child_choices; k++) {
						SenseChoice* mod_choice =
							gu_buf_index(dtree->child[i]->choices, SenseChoice, k);

						ProbCount* pc = mod_choice->prob_counts[j];

						prob_t p1 = prob + pc->prob;
						prob_t p2 = p1   + mod_choice->prob;
						child_outside_probs[k] = log_add(child_outside_probs[k],p1);
						pc->count = log_add(pc->count,p2);
					}
				}
			}
		} else {
			prob_t sum = tree_sum_estimation(dtree->child[i], log_add);
			for (size_t k = 0; k < n_child_choices; k++) {
				child_outside_probs[k] = -sum;
			}
		}

		tree_counting(state, dtree->child[i], child_outside_probs);
	}
}

prob_t
em_step(EMState *state)
{
	// Normalize counts to probabilities
	size_t n_pcs = gu_buf_length(state->pcs);
	for (size_t i = 0; i < n_pcs; i++) {
		ProbCount* pc =
			gu_buf_get(state->pcs, ProbCount*, i);
		pc->prob  = pc->count;
		pc->count = INFINITY;
	}

	// Estimate the new counts
	prob_t corpus_prob = state->bigram_total*log(state->bigram_total);
	for (int i = 0; i < gu_buf_length(state->dtrees); i++) {
		DepTree* dtree = gu_buf_get(state->dtrees, DepTree*, i);

		tree_estimation(dtree, log_max);

		prob_t sum = tree_sum_estimation(dtree, log_add);

		corpus_prob += sum;

		size_t n_choices = gu_buf_length(dtree->choices);
		prob_t outside_probs[n_choices];
		for (size_t j = 0; j < n_choices; j++) {
			outside_probs[j] = -sum;
		}

		tree_counting(state, dtree, outside_probs);
	}
	
	// return the new corpus probability
	return corpus_prob;
}

typedef struct {
	GuMapItor clo1;
	GuMapItor clo2;
	EMState *state;
	GuMap *cat_probs;
	FILE *funigram, *fbigram;
	FunStats* head_stats;
} DumpIter;

static void
collect_cat_probs(GuMapItor* itor, const void* key, void* value, GuExn* err)
{
	DumpIter* self = gu_container(itor, DumpIter, clo1);
	FunStats* head_stats = *((FunStats**) value);
	
	PgfType *ty =
		pgf_function_type(self->state->pgf, head_stats->fun);
	prob_t *pcount =
		gu_map_insert(self->cat_probs, ty->cid);
	*pcount = log_add(*pcount, log_add(head_stats->pc.prob,self->state->unigram_smoothing));
}

static void
dump_mods(GuMapItor* itor, const void* key, void* value, GuExn* err)
{
	DumpIter* self = gu_container(itor, DumpIter, clo2);
	PgfCId mod = key;

	ProbCount* pc = *((ProbCount**) value);
	
	double val = exp(-pc->prob)/self->state->bigram_total;
	if (val*self->state->bigram_total > 0.00001)
		fprintf(self->fbigram, "%s\t%s\t%e\n",
		                       self->head_stats->fun, mod, 
		                       val);
}

static void
dump_heads(GuMapItor* itor, const void* key, void* value, GuExn* err)
{
	DumpIter* self = gu_container(itor, DumpIter, clo1);
	FunStats* head_stats = *((FunStats**) value);

	self->head_stats = head_stats;

	PgfType *ty =
		pgf_function_type(self->state->pgf, head_stats->fun);

	double val = exp(gu_map_get(self->cat_probs, ty->cid, prob_t)-log_add(head_stats->pc.prob,self->state->unigram_smoothing));
	fprintf(self->funigram, "%s\t%e\n", head_stats->fun, val);

	gu_map_iter(self->head_stats->mods, &self->clo2, err);
}

static void
dump_cats(GuMapItor* itor, const void* key, void* value, GuExn* err)
{
	DumpIter* self = gu_container(itor, DumpIter, clo1);
	PgfCId cat  = (PgfCId) key;
	prob_t prob = *((prob_t*) value);

	fprintf(self->funigram, "%s\t%e\n", cat, exp(-prob));
}

void
em_dump(EMState *state, char* unigram_path, char* bigram_path)
{
	GuPool* tmp_pool = gu_local_pool();

	prob_t inf = INFINITY;

	DumpIter itor;
	itor.state = state;
	itor.cat_probs =
		gu_new_string_map(prob_t, &inf, tmp_pool);

	itor.clo1.fn = collect_cat_probs;
	gu_map_iter(state->stats, &itor.clo1, NULL);

	itor.funigram = fopen(unigram_path, "w+");	
	itor.fbigram = fopen(bigram_path, "w+");

	itor.clo1.fn = dump_heads;
	itor.clo2.fn = dump_mods;
	gu_map_iter(state->stats, &itor.clo1, NULL);

	itor.clo1.fn = dump_cats;
	gu_map_iter(itor.cat_probs, &itor.clo1, NULL);

	fclose(itor.fbigram);
	fclose(itor.funigram);

	gu_pool_free(tmp_pool);
}

static GuString
get_field(CONLLFields* fields, size_t i)
{
	return (fields ? (*fields)[i] : "_");
}

typedef struct {
	PgfCId fun;
	prob_t prob;
} LemmaProb;

static int cmp_lemma_prob(const void *p1, const void *p2)
{
	LemmaProb *lp1 = (LemmaProb *) p1;
	LemmaProb *lp2 = (LemmaProb *) p2;
	if (lp1->prob < lp2->prob)
	  return -1;
	else if (lp1->prob > lp2->prob)
	  return 1;
	else
	  return 0;
}

static void
print_annotated_head(FILE* out, DepTree* dtree,
                     size_t parent_index, prob_t* outside_probs,
                     GuBuf* buf)
{
	CONLLFields* fields =
		buf ? gu_buf_index(buf, CONLLFields, dtree->index) : NULL;

	fprintf(out, "%ld\t%s\t%s\t%s\t%s\t%s\t%ld\t%s\t%s\t%s\t",
				 dtree->index+1,
				 get_field(fields,1),
				 get_field(fields,2),
				 get_field(fields,3),
				 get_field(fields,4),
				 get_field(fields,5),
				 parent_index,
				 get_field(fields,7),
				 get_field(fields,8),
				 get_field(fields,9));

	size_t n_choices = gu_buf_length(dtree->choices);
	if (n_choices > 0) {
		LemmaProb choices[n_choices];
		for (size_t j = 0; j < n_choices; j++) {
			SenseChoice* choice =
				gu_buf_index(dtree->choices, SenseChoice, j);
			choices[j].fun  = choice->stats->fun;
			choices[j].prob = outside_probs[j]+choice->prob;
		}
		qsort(choices, n_choices, sizeof(LemmaProb), cmp_lemma_prob);

		int first = 0;
		prob_t best_prob;
		for (size_t j = 0; j < n_choices; j++) {
			switch (first) {
			case 0:
				best_prob = choices[j].prob;
				first = 1;
				break;
			case 1:
				if (choices[j].prob > best_prob) {
					first = 2;
					fputs(" |", out);
				}
			case 2:
				fputc(' ', out);
			}
			fputs(choices[j].fun, out);
		}
	}
	fputc('\n', out);
}

static void
print_annotated_conll_tree(FILE* out, DepTree* dtree,
                           size_t parent_index, prob_t* outside_probs,
                           GuBuf* buf)
{
	size_t n_head_choices = gu_buf_length(dtree->choices);

	bool print_head = true;
	for (size_t i = 0; i < dtree->n_children; i++) {
		if (print_head && dtree->child[i]->index > dtree->index) {
			// print the row of the head
			print_annotated_head(out, dtree,
			                     parent_index, outside_probs, buf);
			print_head = false;
		}

		size_t n_child_choices =
			gu_buf_length(dtree->child[i]->choices);
		prob_t child_outside_probs[n_child_choices];

		if (n_head_choices > 0) {
			for (size_t k = 0; k < n_child_choices; k++) {
				child_outside_probs[k] = INFINITY;
			}

			for (size_t j = 0; j < n_head_choices; j++) {
				SenseChoice* head_choice =
					gu_buf_index(dtree->choices, SenseChoice, j);

				prob_t prob =
					outside_probs[j] + head_choice->prob -
					tree_edge_estimation(j, dtree->child[i], log_max);

				for (size_t k = 0; k < n_child_choices; k++) {
					SenseChoice* mod_choice =
						gu_buf_index(dtree->child[i]->choices, SenseChoice, k);

					ProbCount* pc = mod_choice->prob_counts[j];

					prob_t p1 = prob + pc->prob;
					child_outside_probs[k] = log_max(child_outside_probs[k],p1);
				}
			}
		} else {
			prob_t sum = tree_sum_estimation(dtree->child[i], log_max);
			for (size_t k = 0; k < n_child_choices; k++) {
				child_outside_probs[k] = -sum;
			}
		}

		print_annotated_conll_tree(out, dtree->child[i],
		                           dtree->index+1, child_outside_probs,
		                           buf);
	}
	
	if (print_head) {
		// print the row of the head
		print_annotated_head(out, dtree,
		                     parent_index, outside_probs, buf);
		print_head = false;
	}
}

int
em_export_annotated_treebank(EMState* state, GuString fpath)
{
	FILE *out;
	if (fpath == NULL || *fpath == 0)
		out = stdout;
	else {
		out = fopen(fpath, "w+");
		if (out == NULL) {
			return 0;
		}
	}

	size_t n_trees = gu_buf_length(state->dtrees);
	for (size_t i = 0; i < n_trees; i++) {
		DepTree* dtree = gu_buf_get(state->dtrees, DepTree*, i);

		tree_estimation(dtree, log_max);

		prob_t max = tree_sum_estimation(dtree, log_max);
		size_t n_choices = gu_buf_length(dtree->choices);
		prob_t outside_probs[n_choices];
		for (size_t j = 0; j < n_choices; j++) {
			outside_probs[j] = -max;
		}

		GuBuf* buf = state->fields ? gu_buf_get(state->fields, GuBuf*, i) : NULL;
		print_annotated_conll_tree(out, dtree, 0, outside_probs, buf);
		fprintf(out, "\n");
	}

	if (out != stdout)
		fclose(out);

	return 1;
}

static void
em_annotate_dep_tree_(GuBuf* buf, DepTree* dtree, prob_t* outside_probs)
{
	size_t n_head_choices = gu_buf_length(dtree->choices);

	if (n_head_choices > 0) {
		EMLemmaProb* choices = gu_buf_extend_n(buf, n_head_choices);
		for (size_t j = 0; j < n_head_choices; j++) {
			SenseChoice* choice =
				gu_buf_index(dtree->choices, SenseChoice, j);
			choices[j].index= dtree->index;
			choices[j].fun  = choice->stats->fun;
			choices[j].prob = outside_probs[j]+choice->prob;
		}
		qsort(choices, n_head_choices, sizeof(EMLemmaProb), cmp_lemma_prob);
	}

	for (size_t i = 0; i < dtree->n_children; i++) {
		size_t n_child_choices =
			gu_buf_length(dtree->child[i]->choices);
		prob_t child_outside_probs[n_child_choices];

		if (n_head_choices > 0) {
			for (size_t k = 0; k < n_child_choices; k++) {
				child_outside_probs[k] = INFINITY;
			}

			for (size_t j = 0; j < n_head_choices; j++) {
				SenseChoice* head_choice =
					gu_buf_index(dtree->choices, SenseChoice, j);

				prob_t prob =
					outside_probs[j] + head_choice->prob -
					tree_edge_estimation(j, dtree->child[i], log_max);

				for (size_t k = 0; k < n_child_choices; k++) {
					SenseChoice* mod_choice =
						gu_buf_index(dtree->child[i]->choices, SenseChoice, k);

					ProbCount* pc = mod_choice->prob_counts[j];

					prob_t p1 = prob + pc->prob;
					child_outside_probs[k] = log_max(child_outside_probs[k],p1);
				}
			}
		} else {
			prob_t sum = tree_sum_estimation(dtree->child[i], log_max);
			for (size_t k = 0; k < n_child_choices; k++) {
				child_outside_probs[k] = -sum;
			}
		}

		em_annotate_dep_tree_(buf, dtree->child[i], child_outside_probs);
	}
}

GuBuf*
em_annotate_dep_tree(DepTree* dtree, GuPool* pool)
{
	tree_estimation(dtree, log_max);

	prob_t max = tree_sum_estimation(dtree, log_max);
	size_t n_choices = gu_buf_length(dtree->choices);
	prob_t outside_probs[n_choices];
	for (size_t j = 0; j < n_choices; j++) {
		outside_probs[j] = -max;
	}

	GuBuf* buf = gu_new_buf(EMLemmaProb, pool);
	em_annotate_dep_tree_(buf, dtree, outside_probs);
	return buf;
}

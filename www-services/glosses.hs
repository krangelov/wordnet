{-# LANGUAGE MonadComprehensions, BangPatterns #-}
import PGF2
import Database.Daison
import SenseSchema
import ContentSchema
import Data.Char
import Data.List(partition,intercalate)
import Data.Maybe
import Data.Either
import Data.Data
import Data.Tree
import System.Directory
import Control.Monad
import qualified Data.Map.Strict as Map
import Debug.Trace

main = do
  cncdefs1 <- fmap (mapMaybe (parseCncSyn "ParseAfr") . lines) $ readFile "WordNetAfr.gf"
  cncdefs2 <- fmap (mapMaybe (parseCncSyn "ParseBul") . lines) $ readFile "WordNetBul.gf"
  cncdefs3 <- fmap (mapMaybe (parseCncSyn "ParseCat") . lines) $ readFile "WordNetCat.gf"
  cncdefs4 <- fmap (mapMaybe (parseCncSyn "ParseChi") . lines) $ readFile "WordNetChi.gf"
  cncdefs5 <- fmap (mapMaybe (parseCncSyn "ParseDut") . lines) $ readFile "WordNetDut.gf"
  cncdefs6 <- fmap (mapMaybe (parseCncSyn "ParseEng") . lines) $ readFile "WordNetEng.gf"
  cncdefs7 <- fmap (mapMaybe (parseCncSyn "ParseEst") . lines) $ readFile "WordNetEst.gf"
  cncdefs8 <- fmap (mapMaybe (parseCncSyn "ParseFin") . lines) $ readFile "WordNetFin.gf"
  cncdefs9 <- fmap (mapMaybe (parseCncSyn "ParseGer") . lines) $ readFile "WordNetGer.gf"
  cncdefs10<- fmap (mapMaybe (parseCncSyn "ParseIta") . lines) $ readFile "WordNetIta.gf"
  cncdefs11<- fmap (mapMaybe (parseCncSyn "ParseKor") . lines) $ readFile "WordNetKor.gf"
  cncdefs12<- fmap (mapMaybe (parseCncSyn "ParseMlt") . lines) $ readFile "WordNetMlt.gf"
  cncdefs13<- fmap (mapMaybe (parseCncSyn "ParsePol") . lines) $ readFile "WordNetPol.gf"
  cncdefs14<- fmap (mapMaybe (parseCncSyn "ParsePor") . lines) $ readFile "WordNetPor.gf"
  cncdefs15<- fmap (mapMaybe (parseCncSyn "ParseSlv") . lines) $ readFile "WordNetSlv.gf"
  cncdefs16<- fmap (mapMaybe (parseCncSyn "ParseSom") . lines) $ readFile "WordNetSom.gf"
  cncdefs17<- fmap (mapMaybe (parseCncSyn "ParseSpa") . lines) $ readFile "WordNetSpa.gf"
  cncdefs18<- fmap (mapMaybe (parseCncSyn "ParseSwe") . lines) $ readFile "WordNetSwe.gf"
  cncdefs19<- fmap (mapMaybe (parseCncSyn "ParseTha") . lines) $ readFile "WordNetTha.gf"
  cncdefs20<- fmap (mapMaybe (parseCncSyn "ParseTur") . lines) $ readFile "WordNetTur.gf"

  let cncdefs = Map.fromListWith (++) (cncdefs1++cncdefs2++cncdefs3++cncdefs4++cncdefs5++cncdefs6++cncdefs7++cncdefs8++cncdefs9++cncdefs10++cncdefs11++cncdefs12++cncdefs13++cncdefs14++cncdefs15++cncdefs16++cncdefs17++cncdefs18++cncdefs19++cncdefs20)

  absdefs <- fmap (mapMaybe parseAbsSyn . lines) $ readFile "WordNet.gf"

  fn_examples <- fmap (parseExamples . lines) $ readFile "examples.txt"

  (taxonomy,lexrels0) <-
     fmap (partitionEithers . map parseTaxonomy . lines) $
       readFile "taxonomy.txt"

  probs <-
     fmap (Map.fromList . map parseProbs . lines) $
       readFile "Parse.uncond.probs"

  let lexrels = (Map.toList . Map.fromListWith (++)) lexrels0

  domain_forest <- fmap (parseDomains [] . lines) $ readFile "domains.txt"

  ls <- fmap lines $ readFile "images.txt"
  let images = parseImages ls

  let db_name = "semantics.db"
  fileExists <- doesFileExist db_name
  when fileExists (removeFile db_name)
  db <- openDB db_name
  runDaison db ReadWriteMode $ do
    createTable examples
    createTable classes
    createTable frames
    ex_keys <- let combine (xs1,ys1) (xs2,ys2) = (xs2++xs1,ys2++ys1)
               in fmap (Map.fromListWith combine) $ insertExamples [] fn_examples

    createTable synsets
    forM taxonomy $ \(key,synset) -> do
       store synsets (Just key) synset

    createTable domains
    ids <- insertDomains Map.empty 0 domain_forest

    createTable lexemes
    let synsetKeys = Map.fromList [(synsetOffset synset, key) | (key,synset) <- taxonomy]
    forM_ absdefs $ \(mb_offset,fun,ds,gloss) -> do
       let (es,fs) = fromMaybe ([],[]) (Map.lookup fun ex_keys)
       mb_synsetid <- case mb_offset >>= flip Map.lookup synsetKeys of
                        Nothing | not (null gloss) -> fmap Just (store synsets Nothing (Synset "" [] [] gloss))
                        mb_id                      -> return mb_id
       insert_ lexemes (Lexeme fun (fromMaybe 0 (Map.lookup fun probs))
                               (Map.findWithDefault [] fun cncdefs)
                               mb_synsetid
                               (map (\d -> fromMaybe (error ("Unknown domain "++d)) (Map.lookup d ids)) ds)
                               (fromMaybe [] (Map.lookup fun images))
                               es fs [])
       return ()

    forM_ lexrels $ \(fun,ptrs) ->
      update lexemes [(id,lex{lex_pointers=ptrs'})
                         | (id,lex) <- fromIndex lexemes_fun (at fun)
                         , ptrs' <- select [(sym,id)
                                             | (sym,fun) <- anyOf ptrs
                                             , id <- from lexemes_fun (at fun)]
                         ]

    createTable updates

  cs <- runDaison db ReadOnlyMode $ 
          query (foldRows accumCounts Map.empty) $ 
            [(drop 5 lang,status)
                       | (_,lex) <- from lexemes everything,
                         (lang,status) <- anyOf (status lex)]
  writeFile "build/status.svg" (renderStatus cs)

  closeDB db

parseAbsSyn l =
  case words l of
    ("fun":fn:_) -> case break (=='\t') l of
                      (l1,'\t':l2) -> let (ds,l3) = splitDomains l2
                                      in Just (Just ((reverse . take 10 . reverse) l1), fn, ds, l3)
                      _            -> Just (Nothing, fn, [], "")
    _            -> Nothing
  where
    splitDomains ('[':cs) = split cs
      where
        trim = reverse . dropWhile isSpace . reverse . dropWhile isSpace

        split cs =
          case break (flip elem ",]") cs of
            (x,',':cs) -> let (xs,cs') = split (dropWhile isSpace cs)
                          in (trim x : xs, dropWhile isSpace cs')
            (x,']':cs) -> let x' = trim x
                          in (if null x' then [] else [x'], dropWhile isSpace cs)
            _          -> ([],       cs)
    splitDomains cs = ([],cs)

parseCncSyn lang l =
  case words l of
    ("lin":fn:"=":ws) | null ws                  -> Nothing
                      | last ws == "--unchecked" -> Just (fn,[(lang,Unchecked) | def <- strip (unwords (init ws))])
                      | last ws == "--guessed"   -> Just (fn,[(lang,Guessed)   | def <- strip (unwords (init ws))])
                      | otherwise                -> Just (fn,[(lang,Checked)   | def <- strip (unwords ws)])
    _                                            -> Nothing
  where
    strip s = 
      let def = (reverse . dropWhile (\c -> isSpace c || c == ';') . reverse) s
      in if def == "variants {}"
           then []
           else [def]

data Entry
  = ClassE String [(String,[String])]
  | FrameE   Expr String [Fun]
  | ExampleE Expr [Fun]

parseExamples []                            = []
parseExamples (l1:l2:l3:l4:l5:l6:ls)
  | take 4 l1 == "abs:" && take 4 l5 == "key:" =
      let (w:ws) = words (drop 5 l5)
          fns    = take (read w) ws
      in case readExpr (drop 5 l1) of
           Just e  -> ExampleE e fns : parseExamples ls
           Nothing -> trace ("FAILED: "++l1) (parseExamples ls)
parseExamples (l1:l2:l3:ls)
  | take 4 l1 == "frm:" && take 4 l2 == "sem:" && take 4 l3 == "key:" =
      case readExpr (drop 5 l1) of
        Just e  -> FrameE e (drop 5 l2) (words (drop 5 l3)) : parseExamples ls
        Nothing -> trace ("FAILED: "++l1) (parseExamples ls)
parseExamples (l1:ls)
  | take 6 l1 == "class:"                      =
      let (vs,ls') = break (\l -> take 5 l /= "role:") ls
      in ClassE (drop 7 l1) (map (toVar . drop 6) vs) : parseExamples ls'
  where
    toVar l = let (v:cs) = words l in (v,cs)
parseExamples (l:ls)                        = parseExamples ls


insertExamples ps []                    = do return []
insertExamples ps (ClassE name vs : es) = case ps of
                                            []                                                   -> do id <- insert_ classes (Class name vs Nothing)
                                                                                                       insertExamples [(name,id)] es
                                            ((name',id'):ps) | take (length name') name == name' -> do id <- insert_ classes (Class name vs (Just id'))
                                                                                                       insertExamples ((name,id) : (name',id') : ps) es
                                                             | otherwise                         -> do insertExamples ps (ClassE name vs : es)
insertExamples ps (FrameE e sem fns : es)=do key <- case ps of
                                                      (_,class_id):_ -> insert_ frames (Frame class_id (snd (last ps)) e sem)
                                                      _              -> fail "Frame without class"
                                             xs  <- insertExamples ps es
                                             return ([(fn,([],[key])) | fn <- fns] ++ xs)
insertExamples ps (ExampleE e fns : es) = do key <- insert_ examples e
                                             xs  <- insertExamples ps es
                                             return ([(fn,([key],[])) | fn <- fns] ++ xs)


parseTaxonomy l
  | isDigit (head id) = Left  (read key_s :: Key Synset, Synset id (readPtrs (\sym id -> (sym,read id)) ws2) (read children_s) gloss)
  | otherwise         = Right (id, readPtrs (,) ws0)
  where
    (id:ws0) = words l
    key_s:children_s:ws1 = ws0
    (ws2,"|":ws3) = break (=="|") ws1
    gloss = unwords ws3

    readPtrs f []            = []
    readPtrs f (sym_s:id:ws) = f sym id:readPtrs f ws
      where
        sym = case sym_s of
                "!"  -> Antonym
                "@"  -> Hypernym 
                "@i" -> InstanceHypernym 
                "~"  -> Hyponym 
                "~i" -> InstanceHyponym 
                "#m" -> MemberHolonym 
                "#s" -> SubstanceHolonym 
                "#p" -> PartHolonym 
                "%m" -> MemberMeronym 
                "%s" -> SubstanceMeronym 
                "%p" -> PartMeronym 
                "="  -> Attribute 
                ";c" -> DomainOfSynset Topic 
                "-c" -> MemberOfDomain Topic 
                ";r" -> DomainOfSynset Region 
                "-r" -> MemberOfDomain Region 
                ";u" -> DomainOfSynset Usage 
                "-u" -> MemberOfDomain Usage 
                "*"  -> Entailment
                ">"  -> Cause
                "^"  -> AlsoSee 
                "$"  -> VerbGroup 
                "&"  -> SimilarTo
                "+"  -> Derived
                "\\" -> Derived
                "<"  -> Participle

parseProbs l = (id, p)
  where
    [id,s] = words l
    p = read s :: Float

parseDomains levels []     = attach levels
  where
    attach ((i,t):(j,Node parent ts):levels)
      | i  >  j   = attach ((j,Node parent (reverseChildren t:ts)):levels)
    attach levels = reverse (map (reverseChildren . snd) levels)
parseDomains levels (l:ls) =
  parseDomains ((i',Node (domain,is_dim) []):attach levels) ls
  where
    (i',domain,is_dim) = stripIndent l

    attach ((i,t):(j,Node parent ts):levels)
      | i' <  i   = attach ((j,Node parent (reverseChildren t:ts)):levels)
    attach ((i,t):(j,Node parent ts):levels)
      | i' == i &&
        i  >  j   = (j,Node parent (reverseChildren t:ts)):levels
    attach levels
      | otherwise = levels

    stripIndent ""       = (0,"",False)
    stripIndent (' ':cs) = let (i,domain,is_dim) = stripIndent cs
                           in (i+1,domain,is_dim)
    stripIndent ('-':cs) = let cs1 = dropWhile isSpace cs
                               rcs = reverse cs1
                               (is_dim,cs2)
                                  | take 1 rcs == "*" = (True,  reverse (dropWhile isSpace (tail rcs)))
                                  | otherwise         = (False, cs1)
                           in (0,cs2,is_dim)

reverseChildren (Node x ts) = Node x (reverse ts)

insertDomains !ids parent []                               = return ids
insertDomains !ids parent (Node (name,is_dim) children:ts) = do
  id  <- insert_ domains (Domain name is_dim parent)
  ids <- insertDomains (Map.insert name id ids) id children
  insertDomains ids parent ts

parseImages ls = 
  Map.fromList [case tsv l of {(id:urls) -> (id,map (\s -> case cosv s of {[_,pg,im] -> (pg,im); _ -> error l}) urls)} | l <- ls]

accumCounts m (lang,status) = Map.alter (Just . add) lang m
  where
    add Nothing                = (0,0,0,0)
    add (Just (!g,!u,!ca,!ce)) = case status of
                                   Guessed   -> (g+0.001,u,ca,ce)
                                   Unchecked -> (g,u+0.001,ca,ce)
                                   Changed   -> (g,u,ca+0.001,ce)
                                   Checked   -> (g,u,ca,ce+0.001)

renderStatus cs =
      let (s1,x,y) = Map.foldlWithKey renderBar  ("",5,0) cs
          (s2,_,_) = Map.foldlWithKey renderLang ("",5,y) cs
      in "<?xml version=\"1.0\" encoding=\"utf-8\"?>"++
         "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""++show x++"\" height=\""++show (y+20)++"\">\n"++
         "<g transform=\"translate(0,"++show y++") scale(1,-1)\">\n"++
         s1++
         "</g>\n"++
         s2++
         "</svg>"
      where
        renderBar (s,x,y) lang (g,u,ca,ce) =
          let bar =
                "<rect x=\""++show x++"\" y=\""++show 0++"\" width=\"30\" height=\""++show g++"\" style=\"fill:red\"/>\n"++
                "<rect x=\""++show x++"\" y=\""++show g++"\" width=\"30\" height=\""++show u++"\" style=\"fill:yellow\"/>\n"++
                "<rect x=\""++show x++"\" y=\""++show (g+u)++"\" width=\"30\" height=\""++show ca++"\" style=\"fill:black\"/>\n"++
                "<rect x=\""++show x++"\" y=\""++show (g+u+ca)++"\" width=\"30\" height=\""++show ce++"\" style=\"fill:green\"/>\n"
          in (bar++s,x+35,max y (g+u+ca+ce))

        renderLang (s,x,y) lang (g,u,ca,ce) =
          let text =
                "<text x=\""++show (x+3)++"\" y=\""++show (y+15)++"\">"++lang++"</text>"
          in (text++s,x+35,y)

tsv :: String -> [String]
tsv "" = []
tsv cs =
  let (x,cs1) = break (=='\t') cs
  in x : if null cs1 then [] else tsv (tail cs1)

cosv :: String -> [String]
cosv "" = []
cosv cs =
  let (x,cs1) = break (==';') cs
  in x : if null cs1 then [] else cosv (tail cs1)

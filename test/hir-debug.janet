(import /build/jit :as jit)

(defn format-op [op]
  (match (get op 0)
    :unused "_"
    :unknown "?"
    :imms (string/format "%di" (get op 1))
    :immus (string/format "%du" (get op 1))
    :vreg (string/format "v%d" (get op 1))
    :bb (string/format "bb%d" (get op 1))
    ))

(defn format-arg [arg]
  (def [type val] arg)
  (match type
    :unused nil
    :unknown nil
    :imms (string/format "%di" val)
    :immus (string/format "%du" val)
    :jimm (string/format "%p" val)
    :vreg (string/format "v%d" val)
    :bb (string/format "bb%d" val)
    ))

(defn format-args [args]
  (var formatted-args "")
  (string/join (filter |(= (type $) :string) (map format-arg args)) ", "))

(defn pretty-bbs [bbs]
  (for bb-i 0 (length bbs)
    (print "---- basic block " bb-i " -----")
    (for instr-i 0 (length (get bbs bb-i))
      (def instr (get (get bbs bb-i) instr-i))
      (def { :type type :result result :args args } instr)

      (def result (format-op result))
      (print result " = " type "(" (format-args args) ")")
    )))

(defn my-fib [n]
  (if (< n 2)
    n
    (+ (my-fib (- n 1)) (my-fib (- n 2)))
  ))

(def fib-jit (jit/jitable my-fib))

(pp (get fib-jit :hir))

(pretty-bbs (get fib-jit :hir))

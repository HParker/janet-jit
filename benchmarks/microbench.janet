(import /build/jit :as jit)

(defn jit-perf-cmp [loops f & args]
  (def warmup-itterations 100)
  (def itterations loops)
  (def jit-f (jit/jitable f))

  (for _ 0 warmup-itterations
    (jit-f ;args))

  (if (not (jit/compiled? jit-f))
    (error "JIT fail to compile"))

  (def interp-start (os/clock :monotonic))
  (for _ 0 itterations
    (f ;args))
  (def interp-elapsed (- (os/clock :monotonic) interp-start))

  (def jit-start (os/clock :monotonic))
  (for _ 0 itterations
    (jit-f ;args))
  (def jit-elapsed (- (os/clock :monotonic) jit-start))

  (prin (string/format "| %.04f ips | %.04f ellapsed | " (/ itterations interp-elapsed) interp-elapsed))
  (print (string/format "%.04f ips | %.04f ellapsed  | %.02fx faster |" (/ itterations jit-elapsed) jit-elapsed  (/ interp-elapsed jit-elapsed)))
  # (print "interp ips: " (/ itterations interp-elapsed) " total time: " interp-elapsed)
  # (print "jit ips: " (/ itterations jit-elapsed) " total time: " jit-elapsed)
  )

(defn lerp
  [a b t]
  (+ a (* t (- b a))))

(defn fade [t]
  (- (* 6 (* t t t t t))
     (* 15 (* t t t t))
     (* -10 (* t t t))))

(defn dot
  [x1 y1 x2 y2]
  (+ (* x1 x2) (* y1 y2) ))

(print "------------------------------------------------------------------------")
(print "| name | interpreter ips | interpreter elapsed | jit ips | jit elapsed | times faster |")
(prin "| lerp ")
(jit-perf-cmp 100000000 lerp 1 300 0.765)
(prin "| fade ")
(jit-perf-cmp 100000000 fade 20)
(prin "| dot  ")
(jit-perf-cmp 100000000 dot 20.234 30.567 1.23 9.87)


# Keep track of performance difference when returning to the interpreter
(def perm-table-row
  [151 160 137  91  90  15 131  13 201  95  96  53 194 233   7 225])

(defn perm-in [] (in perm-table-row 6))

(defn perm-get [] (get perm-table-row 6))

(defn my-compare [] (cmp 10 20))

(prin "| in   ")
(jit-perf-cmp 100000000 perm-in)
(prin "| get  ")
(jit-perf-cmp 100000000 perm-get)
(prin "| cmp  ")
(jit-perf-cmp 100000000 my-compare)

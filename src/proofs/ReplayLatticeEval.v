(*
  Copyright 2026 PolitePixels Limited

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*)

Require Import ReplayLattice.
Require Import List Arith Lia.
Import ListNotations.

(**
  [msched] records the execution mode, tile size and worker count. [mkSched] takes
  those three values in that order. The oracle mode selects the serial reference.
*)
Record msched : Type := mkSched {
  (**
    [smode] identifies the execution mode. [ORACLE_MODE] selects the serial
    reference.
  *)
  smode    : nat;

  (**
    [stile] records the traversal tile size in the model schedule.
  *)
  stile    : nat;

  (**
    [sworkers] records the worker count available to schedule-dependent kernels.
  *)
  sworkers : nat
}.

Section Kernels.

(**
  [A] is the element type shared by the generic fold and scan kernels.
*)
Variable A : Type.

(**
  [f] combines the running accumulator with the next operand. The generic kernels
  place no associativity requirement on this operation.
*)
Variable f : A -> A -> A.

(**
  [seed] is the initial accumulator used for whole folds and individual blocks.
*)
Variable seed : A.

(**
  [chain] folds the input from left to right using the section's combining operation.

  Takes [acc] ([A]) which is the initial accumulator.
  Takes [l] ([list A]) which holds the operands in evaluation order.
*)
Fixpoint chain (acc : A) (l : list A) : A :=
  match l with
  | []      => acc
  | x :: r  => chain (f acc x) r
  end.

(**
  [chunks] takes successive prefixes of the input. With positive width and sufficient
  fuel, concatenating the chunks recovers the original input in order.

  Takes [fuel] ([nat]) which is the maximum number of chunks to produce.
  Takes [width] ([nat]) which is the requested number of elements per chunk.
  Takes [xs] ([list A]) which is the input in evaluation order.
*)
Fixpoint chunks (fuel width : nat) (xs : list A) : list (list A) :=
  match fuel with
  | O => []
  | S fuel' =>
      match xs with
      | [] => []
      | _  => firstn width xs :: chunks fuel' width (skipn width xs)
      end
  end.

(**
  [blocked] folds each chunk from the seed and then folds the chunk totals from the
  same seed.

  Takes [k] ([nat]) which is the chunk width.
  Takes [l] ([list A]) which holds the operands in evaluation order.
*)
Definition blocked (k : nat) (l : list A) : A :=
  chain seed (map (chain seed) (chunks (length l) k l)).

(**
  [EVAL_BLOCK] fixes the replay reduction's leaf width at 64 elements.
*)
Definition EVAL_BLOCK : nat := 64.

(**
  [kernel_fixed_tree] uses the same block boundaries and merge order on every
  schedule.

  Takes [s] ([msched]) which is the schedule supplied to the kernel.
  Takes [l] ([list A]) which holds the operands in input order.
*)
Definition kernel_fixed_tree (s : msched) (l : list A) : A := blocked EVAL_BLOCK l.

(**
  [row_fixed_tree_reduction] proves that changing schedules preserves the fixed
  tree's result for any combining operation.

  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Theorem row_fixed_tree_reduction :
  forall s s' l, kernel_fixed_tree s l = kernel_fixed_tree s' l.
Proof. reflexivity. Qed.

(**
  [kernel_tree_by_workers] sets the leaf width to one more than the worker count so
  that the schedule can change the tree.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_tree_by_workers (s : msched) (l : list A) : A := blocked (S (sworkers s)) l.

(**
  [kernel_move] models data movement by returning the input value on every schedule.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [x] ([A]) which is the input value.
*)
Definition kernel_move (s : msched) (x : A) : A := x.

(**
  [row_data_movement] proves equality of the moved value across schedules.

  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [x] ([A]) which is the input value.
*)
Theorem row_data_movement :
  forall s s' x, kernel_move s x = kernel_move s' x.
Proof. reflexivity. Qed.

(**
  [kernel_move_by_workers] selects a different input when the worker count changes
  from zero to a positive value.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [x] and [y] ([A]) which are the values selected for zero and positive worker
  counts respectively.
*)
Definition kernel_move_by_workers (s : msched) (x y : A) : A :=
  match sworkers s with O => x | S _ => y end.

(**
  [kernel_recurrence] carries the preceding accumulator into the next update in
  input order.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_recurrence (s : msched) (l : list A) : A := chain seed l.

(**
  [row_carried_recurrence] proves schedule agreement for the recurrence with a fixed
  seed and update order.

  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Theorem row_carried_recurrence :
  forall s s' l, kernel_recurrence s l = kernel_recurrence s' l.
Proof. reflexivity. Qed.

(**
  [kernel_recurrence_by_workers] lets the worker count change the initial
  accumulator before the first update.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_recurrence_by_workers (s : msched) (l : list A) : A :=
  chain (match sworkers s with O => seed | S _ => f seed seed end) l.

(**
  [free_width] divides the input length by the worker count, rounding up. The width
  and effective worker count are each at least one.

  Takes [s] ([msched]) which supplies the requested worker count.
  Takes [n] ([nat]) which is the number of input elements.
*)
Definition free_width (s : msched) (n : nat) : nat :=
  let lanes := Nat.max 1 (sworkers s) in Nat.max 1 ((n + lanes - 1) / lanes).

(**
  [kernel_free] uses a block width derived from the worker count. This is one
  permitted grouping for an approximate reduction in the model.

  Takes [s] ([msched]) which supplies the requested worker count.
  Takes [l] ([list A]) which holds the operands in input order.
*)
Definition kernel_free (s : msched) (l : list A) : A := blocked (free_width s (length l)) l.

(**
  [SCANW] fixes the leaf width of the blocked scan independently of the worker count.
*)
Definition SCANW : nat := 64.

(**
  [scan_from] produces the successive accumulators of a left-to-right scan, starting
  with the first update.

  Takes [acc] ([A]) which is the accumulator on entry.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Fixpoint scan_from (acc : A) (l : list A) : list A :=
  match l with
  | []     => []
  | x :: r => let v := f acc x in v :: scan_from v r
  end.

(**
  [scan_over] forms each prefix from its block-local prefix and the accumulator on
  entry to that block.

  Takes [acc] ([A]) which is the total carried into the first block.
  Takes [cs] ([list (list A)]) which holds the blocks in input order.
*)
Fixpoint scan_over (acc : A) (cs : list (list A)) : list A :=
  match cs with
  | []        => []
  | c :: rest => map (fun local => f acc local) (scan_from seed c)
                   ++ scan_over (f acc (chain seed c)) rest
  end.

(**
  [scan_blocked] scans each block locally and combines its prefixes with the
  incoming total.

  Takes [k] ([nat]) which is the block width.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition scan_blocked (k : nat) (l : list A) : list A :=
  scan_over seed (chunks (length l) k l).

(**
  [kernel_scan_fixed] uses the same block width for every schedule.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_scan_fixed (s : msched) (l : list A) : list A := scan_blocked SCANW l.

(**
  [kernel_scan_by_workers] lets the worker count determine the scan's block width.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_scan_by_workers (s : msched) (l : list A) : list A :=
  scan_blocked (free_width s (length l)) l.

(**
  [row_fixed_block_scan] proves that every prefix of the fixed-width scan agrees
  across schedules.

  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Theorem row_fixed_block_scan :
  forall s s' l, kernel_scan_fixed s l = kernel_scan_fixed s' l.
Proof. reflexivity. Qed.

End Kernels.

(**
  [wf2] doubles the left operand before adding the right. The scan and reduction
  witnesses use it to distinguish different groupings of the same input.

  Takes [a] and [b] ([nat]) which are the left and right operands respectively.
*)
Definition wf2 (a b : nat) : nat := 2 * a + b.

(**
  [witness_scan_by_workers_moves] exhibits an input whose scan under [wf2] gives
  different prefix values for two block widths.
*)
Theorem witness_scan_by_workers_moves :
  exists l s s',
    kernel_scan_by_workers nat wf2 0 s l <> kernel_scan_by_workers nat wf2 0 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 1), (mkSched 0 0 2).
  vm_compute.
  discriminate.
Qed.

(**
  [witness_free_reduction_moves] exhibits two schedules whose permitted groupings
  give different fold results for the same input.
*)
Theorem witness_free_reduction_moves :
  exists l s s',
    kernel_free nat wf2 0 s l <> kernel_free nat wf2 0 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 1), (mkSched 0 0 2).
  vm_compute.
  discriminate.
Qed.

(**
  [witness_tree_by_workers_moves] exhibits a change in the tree's result when the
  leaf width follows the worker count.
*)
Theorem witness_tree_by_workers_moves :
  exists l s s',
    kernel_tree_by_workers nat wf2 0 s l <> kernel_tree_by_workers nat wf2 0 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute.
  discriminate.
Qed.

(**
  [witness_move_by_workers_moves] exhibits unequal results when the move selects its
  input by worker count.
*)
Theorem witness_move_by_workers_moves :
  exists x y s s',
    kernel_move_by_workers nat s x y <> kernel_move_by_workers nat s' x y.
Proof.
  exists 0, 1, (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute.
  discriminate.
Qed.

(**
  [witness_recurrence_by_workers_moves] exhibits a recurrence whose result changes
  when the worker count changes its initial accumulator.
*)
Theorem witness_recurrence_by_workers_moves :
  exists l s s',
    kernel_recurrence_by_workers nat wf2 1 s l <> kernel_recurrence_by_workers nat wf2 1 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute.
  discriminate.
Qed.

(**
  [approx_rows_are_vacuous] records why an [Approx] operation imposes no schedule
  comparison. Its schedule-agreement premise is false for every pair.

  Takes [P] ([prog]) which is the graph containing the approximate operation.
  Takes [i] ([nat]) which is the index of the node under consideration.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] and [s'] ([schedule]) which are the schedules in the agreement premise.
  Takes [w] ([world]) which is the external state shared by both runs.
*)
Theorem approx_rows_are_vacuous :
  forall (P : prog) i n s s' w,
    nth_error P i = Some n ->
    op_lane n = Approx ->
    agrees (op_lane n) s s' ->
    eval P i s w = eval P i s' w.
Proof.
  intros P i n s s' w _ Hop Hag.
  rewrite Hop in Hag.
  simpl in Hag.
  contradiction.
Qed.

(**
  [kernel_source] models a read of the value supplied by a fixed external world.

  Takes [A] ([Type]) which is the element type.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [x] ([A]) which is the value supplied by the source.
*)
Definition kernel_source (A : Type) (s : msched) (x : A) : A := x.

(**
  [row_external_source_at_fixed_world] proves schedule agreement when the external
  source supplies the same value to both runs.

  Takes [A] ([Type]) which is the element type.
  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [x] ([A]) which is the common source value.
*)
Theorem row_external_source_at_fixed_world :
  forall (A : Type) s s' (x : A), kernel_source A s x = kernel_source A s' x.
Proof. reflexivity. Qed.

(**
  [source_by_workers] models a source whose value depends on the worker count.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [x] and [y] ([nat]) which are the values selected for zero and positive
  worker counts respectively.
*)
Definition source_by_workers (s : msched) (x y : nat) : nat :=
  match sworkers s with O => x | S _ => y end.

(**
  [witness_source_by_workers_moves] exhibits two schedules that read different
  values from the worker-dependent source.
*)
Theorem witness_source_by_workers_moves :
  exists x y s s', source_by_workers s x y <> source_by_workers s' x y.
Proof.
  exists 0, 1, (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute.
  discriminate.
Qed.

(**
  [reference] defines the serial reference as a left-to-right fold from the seed.

  Takes [A] ([Type]) which is the element type.
  Takes [f] ([A -> A -> A]) which combines an accumulator with the next operand.
  Takes [seed] ([A]) which is the initial accumulator.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition reference (A : Type) (f : A -> A -> A) (seed : A) (l : list A) : A :=
  chain A f seed l.

(**
  [kernel_oracle] evaluates the fold in reference order for every supplied schedule.

  Takes [A] ([Type]) which is the element type.
  Takes [f] ([A -> A -> A]) which combines an accumulator with the next operand.
  Takes [seed] ([A]) which is the initial accumulator.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_oracle (A : Type) (f : A -> A -> A) (seed : A)
                         (s : msched) (l : list A) : A := chain A f seed l.

(**
  [row_oracle_order_matches_reference] proves equality with the serial result for
  any combining operation.

  Takes [A] ([Type]) which is the element type.
  Takes [f] ([A -> A -> A]) which combines an accumulator with the next operand.
  Takes [seed] ([A]) which is the initial accumulator.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Theorem row_oracle_order_matches_reference :
  forall A f seed s l, kernel_oracle A f seed s l = reference A f seed l.
Proof. reflexivity. Qed.

(**
  [witness_fixed_tree_is_not_reference] exhibits an input on which the fixed tree and
  the serial fold give different natural-number results with [wf2].
*)
Theorem witness_fixed_tree_is_not_reference :
  exists l, kernel_fixed_tree nat wf2 0 (mkSched 0 0 0) l <> reference nat wf2 0 l.
Proof.
  exists (repeat 0 63 ++ [1; 0; 0]).
  vm_compute.
  discriminate.
Qed.

(**
  [kernel_elementwise] applies the same operation independently at every position
  and preserves input order.

  Takes [A] and [B] ([Type]) which are the input and output element types
  respectively.
  Takes [g] ([A -> B]) which is the operation applied to each element.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Definition kernel_elementwise (A B : Type) (g : A -> B) (s : msched) (l : list A) : list B :=
  map g l.

(**
  [row_elementwise] proves that the ordered list of elementwise results is
  independent of the schedule.

  Takes [A] and [B] ([Type]) which are the input and output element types
  respectively.
  Takes [g] ([A -> B]) which is the operation applied to each element.
  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Theorem row_elementwise :
  forall A B (g : A -> B) s s' l,
    kernel_elementwise A B g s l = kernel_elementwise A B g s' l.
Proof. reflexivity. Qed.

(**
  [elementwise_by_workers] increments each element only when the schedule has a
  positive worker count.

  Takes [s] ([msched]) which is the execution schedule.
  Takes [l] ([list nat]) which holds the input elements in order.
*)
Definition elementwise_by_workers (s : msched) (l : list nat) : list nat :=
  map (fun x => match sworkers s with O => x | S _ => S x end) l.

(**
  [witness_elementwise_by_workers_moves] exhibits a schedule change that alters the
  result of the worker-dependent elementwise operation.
*)
Theorem witness_elementwise_by_workers_moves :
  exists l s s', elementwise_by_workers s l <> elementwise_by_workers s' l.
Proof.
  exists [0], (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute.
  discriminate.
Qed.

Section ExactInteger.

(**
  [A] is the element type on which the exact-reduction laws are assumed.
*)
Variable A : Type.

(**
  [g] is the binary operation whose blocked and serial folds are compared.
*)
Variable g : A -> A -> A.

(**
  [e] is the identity used to start the serial fold and each block fold.
*)
Variable e : A.

(**
  [dom] identifies the elements on which the reduction is closed and associative.
*)
Variable dom : A -> Prop.

(**
  [dom_e] requires the identity element to belong to the reduction domain.
*)
Hypothesis dom_e : dom e.

(**
  [dom_closed] requires the combination of any two elements in the domain to stay in
  the domain.

  Takes [a] and [b] ([A]) which are the domain elements to combine.
*)
Hypothesis dom_closed : forall a b, dom a -> dom b -> dom (g a b).

(**
  [g_assoc] permits a change of brackets when all three operands belong to the
  domain.

  Takes [a], [b] and [c] ([A]) which are the three domain elements in operand order.
*)
Hypothesis g_assoc : forall a b c, dom a -> dom b -> dom c -> g (g a b) c = g a (g b c).

(**
  [g_left] makes the seed a left identity for every element.

  Takes [a] ([A]) which is the operand to the right of the identity.
*)
Hypothesis g_left  : forall a, g e a = a.

(**
  [g_right] makes the seed a right identity for every element.

  Takes [a] ([A]) which is the operand to the left of the identity.
*)
Hypothesis g_right : forall a, g a e = a.

(**
  [dom_chain] proves domain closure for the result of a fold when its seed and every
  operand belong to the domain.

  Takes [l] ([list A]) which holds the input elements in order.
  Takes [acc] ([A]) which is the accumulator on entry.
*)
Lemma dom_chain : forall l acc, dom acc -> Forall dom l -> dom (chain A g acc l).
Proof.
  induction l as [| x r IH]; intros acc Hacc Hl; simpl.
  - exact Hacc.
  - inversion Hl as [| ? ? Hx Hr]; subst.
    apply IH.
    + apply dom_closed; assumption.
    + exact Hr.
Qed.

(**
  [chain_acc] factors the incoming accumulator out of a fold when the accumulator
  and every input belong to the domain.

  Takes [l] ([list A]) which holds the input elements in order.
  Takes [acc] ([A]) which is the accumulator on entry.
*)
Lemma chain_acc : forall l acc,
  dom acc -> Forall dom l -> chain A g acc l = g acc (chain A g e l).
Proof.
  induction l as [| x r IH]; intros acc Hacc Hl; simpl.
  - rewrite g_right.
    reflexivity.
  - inversion Hl as [| ? ? Hx Hr]; subst.
    rewrite (IH (g acc x) (dom_closed _ _ Hacc Hx) Hr).
    rewrite (IH (g e x) (dom_closed _ _ dom_e Hx) Hr).
    rewrite g_left.

    rewrite g_assoc.
    + reflexivity.
    + assumption.
    + assumption.
    + apply dom_chain.
      * exact dom_e.
      * exact Hr.
Qed.

(**
  [chain_app] combines the folds of two input segments to obtain the fold of their
  concatenation. Both segments must lie in the domain.

  Takes [l1] and [l2] ([list A]) which are the first and second input segments
  respectively.
*)
Lemma chain_app : forall l1 l2,
  Forall dom l1 -> Forall dom l2 ->
  chain A g e (l1 ++ l2) = g (chain A g e l1) (chain A g e l2).
Proof.
  intros l1 l2 H1 H2.
  assert (Hsplit : forall l1' acc,
    chain A g acc (l1' ++ l2) = chain A g (chain A g acc l1') l2).
  { induction l1' as [| x r IH]; intros acc; simpl.
    - reflexivity.
    - apply IH. }

  rewrite Hsplit, (chain_acc l2 (chain A g e l1)).
  - reflexivity.
  - apply dom_chain.
    + exact dom_e.
    + exact H1.
  - exact H2.
Qed.

(**
  [chunks_concat] recovers the original input by concatenating its chunks when the
  width is positive and the fuel is at least the input length.

  Takes [fuel] ([nat]) which is the number of chunks the recursion may produce.
  Takes [k] ([nat]) which is the block width.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Lemma chunks_concat : forall fuel k l,
  1 <= k -> length l <= fuel -> concat (chunks A fuel k l) = l.
Proof.
  induction fuel as [| fuel IH]; intros k l Hk Hlen; simpl.
  - destruct l.
    + reflexivity.
    + simpl in Hlen.
      lia.
  - destruct l as [| x r].
    { reflexivity. }
    simpl.
    rewrite IH.
    + apply firstn_skipn.
    + exact Hk.
    + rewrite length_skipn.
      lia.
Qed.

(**
  [forall_firstn] preserves domain membership when taking a prefix of the input.

  Takes [k] ([nat]) which is the requested prefix length.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Lemma forall_firstn : forall k (l : list A), Forall dom l -> Forall dom (firstn k l).
Proof.
  induction k as [| k IH]; intros l Hl; simpl.
  { constructor. }
  destruct l as [| x r].
  { constructor. }
  inversion Hl as [| ? ? Hx Hr]; subst.
  constructor.
  - exact Hx.
  - apply IH.
    exact Hr.
Qed.

(**
  [forall_skipn] preserves domain membership when dropping a prefix of the input.

  Takes [k] ([nat]) which is the requested prefix length.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Lemma forall_skipn : forall k (l : list A), Forall dom l -> Forall dom (skipn k l).
Proof.
  induction k as [| k IH]; intros l Hl; simpl.
  { exact Hl. }
  destruct l as [| x r].
  { constructor. }
  inversion Hl as [| ? ? Hx Hr]; subst.
  apply IH.
  exact Hr.
Qed.

(**
  [chunks_dom] proves that each produced chunk contains only elements of the
  original domain.

  Takes [fuel] ([nat]) which is the number of chunks the recursion may produce.
  Takes [k] ([nat]) which is the block width.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Lemma chunks_dom : forall fuel k l,
  Forall dom l -> Forall (Forall dom) (chunks A fuel k l).
Proof.
  induction fuel as [| fuel IH]; intros k l Hl; simpl.
  { constructor. }
  destruct l as [| x r].
  { constructor. }
  constructor.
  - apply forall_firstn.
    exact Hl.
  - apply IH.
    apply forall_skipn.
    exact Hl.
Qed.

(**
  [concat_dom] proves that concatenating lists of domain elements preserves
  membership throughout the result.

  Takes [ls] ([list (list A)]) which holds the input lists.
*)
Lemma concat_dom : forall ls, Forall (Forall dom) ls -> Forall dom (concat ls).
Proof.
  induction ls as [| c cs IH]; intros H; simpl.
  { constructor. }
  inversion H as [| ? ? Hc Hcs]; subst.
  apply Forall_app.
  split.
  - exact Hc.
  - apply IH.
    exact Hcs.
Qed.

(**
  [chain_map_chunks] shows that for chunks of domain elements, folding the chunk
  totals gives the same result as folding the flattened input. The proof uses
  associativity and the identity laws.

  Takes [ls] ([list (list A)]) which holds the chunks in input order.
*)
Lemma chain_map_chunks : forall ls,
  Forall (Forall dom) ls ->
  chain A g e (map (chain A g e) ls) = chain A g e (concat ls).
Proof.
  induction ls as [| c cs IH]; intros H; simpl.
  { reflexivity. }
  inversion H as [| ? ? Hc Hcs]; subst.
  rewrite chain_app by (exact Hc || apply concat_dom, Hcs).
  rewrite <- IH by exact Hcs.

  rewrite (chain_acc (map (chain A g e) cs) (g e (chain A g e c))).
  - rewrite g_left.
    reflexivity.
  - apply dom_closed.
    + exact dom_e.
    + apply dom_chain.
      * exact dom_e.
      * exact Hc.
  - apply Forall_map.
    apply Forall_impl with (P := Forall dom).
    + intros a Ha.
      apply dom_chain.
      * exact dom_e.
      * exact Ha.
    + exact Hcs.
Qed.

(**
  [row_exact_integer] shows that a blocked fold equals the serial fold for a positive
  block width. The proof assumes an associative operation on a closed domain with an
  identity for all elements of [A]. All input elements must lie in the domain.

  Takes [k] ([nat]) which is the block width.
  Takes [l] ([list A]) which holds the operands in order.
*)
Theorem row_exact_integer :
  forall k l, 1 <= k -> Forall dom l -> blocked A g e k l = chain A g e l.
Proof.
  intros k l Hk Hl.
  unfold blocked.
  rewrite chain_map_chunks by (apply chunks_dom; exact Hl).
  rewrite chunks_concat by (assumption || apply Nat.le_refl).
  reflexivity.
Qed.

(**
  [row_exact_integer_schedule_invariant] applies [row_exact_integer] to show that
  schedule-dependent block widths give the same result for inputs in the domain.

  Takes [s] and [s'] ([msched]) which are the two schedules being compared.
  Takes [l] ([list A]) which holds the input elements in order.
*)
Corollary row_exact_integer_schedule_invariant :
  forall s s' l, Forall dom l -> kernel_free A g e s l = kernel_free A g e s' l.
Proof.
  intros s s' l Hl.
  unfold kernel_free.
  rewrite !row_exact_integer by (apply Nat.le_max_l || exact Hl).
  reflexivity.
Qed.

End ExactInteger.

Require Import ZArith.
Require Import Floats.

(**
  [row_exact_integer_at_Z] instantiates the schedule-invariance result for addition
  on mathematical integers.

  Takes [s] and [s'] ([msched]) which are the schedules whose groupings are compared.
  Takes [l] ([list Z]) which is their common input.
*)
Theorem row_exact_integer_at_Z :
  forall s s' (l : list Z), kernel_free Z Z.add 0%Z s l = kernel_free Z Z.add 0%Z s' l.
Proof.
  intros s s' l.
  apply (row_exact_integer_schedule_invariant Z Z.add 0%Z (fun _ => True));
    try (intros; exact I); try (intros; lia).
  apply Forall_forall.
  intros x _.
  exact I.
Qed.

(**
  [witness_float_reassociation_moves] exhibits three binary64 operands for which
  changing the brackets changes the sum.
*)
Theorem witness_float_reassociation_moves :
  ((0.1 + 0.2) + 0.3 =? 0.1 + (0.2 + 0.3))%float = false.
Proof. vm_compute. reflexivity. Qed.

(**
  [mworld] represents the external world of the three-node witness by a natural
  number.
*)
Definition mworld := nat.

(**
  [mbits] represents evaluator results in the three-node witness by natural numbers.
*)
Definition mbits := nat.

(**
  [ORACLE_MODE] identifies the serial reference mode in the concrete schedule model.
*)
Definition ORACLE_MODE : nat := 3.

(**
  [mgated] recognises model schedules whose mode differs from the serial reference
  mode.

  Takes [s] ([msched]) which is the execution schedule.
*)
Definition mgated (s : msched) : Prop := smode s <> ORACLE_MODE.

(**
  [magrees] instantiates schedule agreement for the model. [Replay] compares
  schedules whose modes are both gated, [Exact] compares every pair and [Approx]
  compares none.

  Takes [l] ([lane]) which is the lane at which the schedules are compared.
  Takes [s] and [s'] ([msched]) which are the schedules of the two runs.
*)
Definition magrees (l : lane) (s s' : msched) : Prop :=
  match l with
  | Exact  => True
  | Replay => mgated s /\ mgated s'
  | Approx => False
  end.

(**
  [meval_at] evaluates a node with natural-number arithmetic. The world supplies the
  seed and the fallback result when fuel runs out or the node is absent.

  Takes [fuel] ([nat]) which is the remaining recursion depth.
  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [i] ([nat]) which identifies the requested node.
  Takes [s] ([msched]) which selects the grouping for each reduction.
  Takes [w] ([mworld]) which is the natural-number world used as the seed.
*)
Fixpoint meval_at (fuel : nat) (P : prog) (i : nat) (s : msched) (w : mworld) : mbits :=
  match fuel with
  | O => w
  | S fuel' =>
      match nth_error P i with
      | None => w
      | Some n =>
          let operands := map (fun j => meval_at fuel' P j s w) (inputs n) in
          let carried_value :=
            match carried n with
            | Some c => meval_at fuel' P c s w
            | None   => 0
            end in
          match op_lane n with
          | Exact  => chain nat Nat.add w (carried_value :: operands)
          | Replay => if Nat.eqb (smode s) ORACLE_MODE then chain nat Nat.add w (carried_value :: operands)
                      else kernel_fixed_tree nat Nat.add w s (carried_value :: operands)
          | Approx => if Nat.eqb (smode s) ORACLE_MODE then chain nat Nat.add w (carried_value :: operands)
                      else kernel_free nat Nat.add w s (carried_value :: operands)
          end
      end
  end.

(**
  [meval] starts the natural-number evaluator with one more unit of fuel than the
  requested index.

  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [i] ([nat]) which identifies the requested node.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [w] ([mworld]) which is the external world of the witness.
*)
Definition meval (P : prog) (i : nat) (s : msched) (w : mworld) : mbits :=
  meval_at (S i) P i s w.

(**
  [demo_prog] places an exact source, a replay node and an approximate node in
  dependency order for the natural-number witness.
*)
Definition demo_prog : prog :=
  [ mkNode Exact  []      None         true  None
  ; mkNode Replay [0]     (Some Replay) false None
  ; mkNode Approx [0; 1]  None         false None ].

(**
  [node_inputs_of] reads the ordinary dependencies from a lookup, returning an empty
  list when the node is absent.

  Takes [o] ([option node]) which is the result of the node lookup.
*)
Definition node_inputs_of (o : option node) : list nat :=
  match o with Some x => inputs x | None => [] end.

(**
  [node_lane_of] reads the operation lane from a lookup, using [Exact] for an absent
  node.

  Takes [o] ([option node]) which is the result of the node lookup.
*)
Definition node_lane_of (o : option node) : lane :=
  match o with Some x => op_lane x | None => Exact end.

(**
  [demo_prog_wf] checks that the ordinary inputs in the three-node witness precede
  their consumers.
*)
Theorem demo_prog_wf : wf demo_prog.
Proof.
  intros i n j Hn Hj.
  apply (f_equal node_inputs_of) in Hn.
  unfold demo_prog in Hn.
  destruct i as [| [| [| i]]]; cbn in Hn; rewrite <- Hn in Hj; cbn in Hj.
  - destruct Hj.
  - destruct Hj as [<- | []]; lia.
  - destruct Hj as [<- | [<- | []]]; lia.
  - destruct i; cbn in Hj; destruct Hj.
Qed.

(**
  [demo_assumption1] establishes Assumption 1 for [meval] on the three nodes of
  [demo_prog]. Schedules that agree at a node's operation lane give equal results
  when its inputs agree.

  Takes [i] ([nat]) which is the node index in the witness graph.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] and [s'] ([msched]) which are the schedules of the two runs.
  Takes [w] ([mworld]) which is the common natural-number world.
*)
Theorem demo_assumption1 :
  forall i n s s' w,
    nth_error demo_prog i = Some n ->
    magrees (op_lane n) s s' ->
    (forall j, In j (inputs n) -> meval demo_prog j s w = meval demo_prog j s' w) ->
    (forall c, carried n = Some c -> meval demo_prog c s w = meval demo_prog c s' w) ->
    meval demo_prog i s w = meval demo_prog i s' w.
Proof.
  intros i n s s' w Hn Hag Hin Hcar.
  assert (Hlane := f_equal node_lane_of Hn).
  unfold demo_prog in Hlane.
  destruct i as [| [| [| i]]]; cbn in Hlane.
  - reflexivity.
  - rewrite <- Hlane in Hag.
    destruct Hag as [Hg Hg'].
    apply Nat.eqb_neq in Hg, Hg'.
    unfold meval.
    cbn [meval_at nth_error demo_prog op_lane inputs carried map].
    rewrite Hg, Hg'.
    reflexivity.
  - rewrite <- Hlane in Hag.
    cbn in Hag.
    contradiction.
  - destruct i; unfold meval; cbn; reflexivity.
Qed.

(**
  [mgated_separates] exhibits both a gated schedule and an ungated schedule in the
  concrete model.
*)
Theorem mgated_separates :
  (exists s, mgated s) /\ (exists s, ~ mgated s).
Proof.
  split.
  - exists (mkSched 0 0 0).
    unfold mgated, ORACLE_MODE.
    discriminate.
  - exists (mkSched ORACLE_MODE 0 0).
    unfold mgated.
    intros H.
    apply H.
    reflexivity.
Qed.

(**
  [combine64] combines binary64 operands in serial order for [Exact] and in oracle
  mode. A gated schedule selects the fixed tree for [Replay] and worker-dependent
  blocking for [Approx].

  Takes [l] ([lane]) which is the operation lane.
  Takes [s] ([msched]) which supplies the mode and worker count.
  Takes [xs] ([list float]) which holds the operands in input order.
*)
Definition combine64 (l : lane) (s : msched) (xs : list float) : float :=
  let serial := reference float PrimFloat.add 0%float xs in
  match l with
  | Exact => serial
  | Replay => if Nat.eqb (smode s) ORACLE_MODE then serial
              else kernel_fixed_tree float PrimFloat.add 0%float s xs
  | Approx => if Nat.eqb (smode s) ORACLE_MODE then serial
              else kernel_free float PrimFloat.add 0%float s xs
  end.

(**
  [eval64_at] reads source values from the world and combines ordinary inputs at
  every other node. An absent node or exhausted fuel yields zero.

  Takes [fuel] ([nat]) which is the remaining recursion depth.
  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [i] ([nat]) which identifies the requested node.
  Takes [s] ([msched]) which selects the reduction schedule.
  Takes [w] ([nat -> float]) which supplies the value at each source index.
*)
Fixpoint eval64_at (fuel : nat) (P : prog) (i : nat) (s : msched) (w : nat -> float) : float :=
  match fuel with
  | O => 0%float
  | S rest =>
      match nth_error P i with
      | None => 0%float
      | Some n => if source n then w i else
          combine64 (op_lane n) s (map (fun j => eval64_at rest P j s w) (inputs n))
      end
  end.

(**
  [eval64] starts the binary64 evaluator with enough fuel to follow the ordinary
  dependencies of a well-formed graph.

  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [i] ([nat]) which identifies the requested node.
*)
Definition eval64 P i := eval64_at (S i) P i.

(**
  [eval64_enough_fuel] shows that once the recursion budget exceeds the node index,
  a different sufficient budget gives the same result on a well-formed graph.

  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [f] and [g] ([nat]) which are the two recursion budgets.
  Takes [i] ([nat]) which identifies the requested node.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [w] ([nat -> float]) which supplies a binary64 value for each source index.
*)
Lemma eval64_enough_fuel : forall P, wf P -> forall f g i s w,
  i < f -> i < g -> eval64_at f P i s w = eval64_at g P i s w.
Proof.
  intros P Hwf f.
  induction f as [| f IH]; intros [| g] i s w Hf Hg; try lia.
  simpl.
  destruct (nth_error P i) as [n |] eqn:Hn.
  - destruct (source n).
    { reflexivity. }
    f_equal.
    apply map_ext_in.
    intros j Hj.
    specialize (Hwf i n j Hn Hj).
    apply IH; lia.
  - reflexivity.
Qed.

(**
  [eval64_node] exposes one step of binary64 evaluation on a well-formed graph. A
  source reads its world value and any other node combines its ordinary inputs.

  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [i] ([nat]) which identifies the requested node.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] ([msched]) which is the execution schedule.
  Takes [w] ([nat -> float]) which supplies a binary64 value for each source index.
*)
Lemma eval64_node : forall P i n s w, wf P -> nth_error P i = Some n ->
  eval64 P i s w = if source n then w i else
    combine64 (op_lane n) s (map (fun j => eval64 P j s w) (inputs n)).
Proof.
  intros P i n s w Hwf Hn.
  unfold eval64 at 1.
  simpl.
  rewrite Hn.
  destruct (source n).
  { reflexivity. }

  f_equal.
  apply map_ext_in.
  intros j Hj.
  apply eval64_enough_fuel.
  - exact Hwf.
  - exact (Hwf i n j Hn Hj).
  - lia.
Qed.

(**
  [binary64_assumption1_on_dags] establishes the local premise of Assumption 1 for
  the binary64 evaluator on well-formed graphs without carried edges. A node gives
  equal results when its inputs are equal and its schedules agree at its operation
  lane.

  Takes [P] ([prog]) which is the graph being evaluated.
  Takes [i] ([nat]) which is the node index whose result is compared.
  Takes [n] ([node]) which is the node at that index.
  Takes [s] and [s'] ([msched]) which are the schedules of the two runs.
  Takes [w] ([nat -> float]) which supplies the same source values to both runs.
*)
Theorem binary64_assumption1_on_dags : forall P, wf P -> acyclic P ->
  forall i n s s' w, nth_error P i = Some n -> magrees (op_lane n) s s' ->
    (forall j, In j (inputs n) -> eval64 P j s w = eval64 P j s' w) ->
    eval64 P i s w = eval64 P i s' w.
Proof.
  intros P Hwf _ i n s s' w Hn Hag Hin.
  rewrite (eval64_node P i n s w Hwf Hn), (eval64_node P i n s' w Hwf Hn).
  destruct (source n).
  { reflexivity. }
  assert (Hxs : map (fun j => eval64 P j s w) (inputs n) =
                map (fun j => eval64 P j s' w) (inputs n)).
  { apply map_ext_in.
    exact Hin. }

  rewrite Hxs.
  destruct (op_lane n); simpl in *.
  - reflexivity.
  - destruct Hag as [H H'].
    unfold mgated in H, H'.
    apply Nat.eqb_neq in H, H'.
    rewrite H, H'.
    apply row_fixed_tree_reduction.
  - contradiction.
Qed.
